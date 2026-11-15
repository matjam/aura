/* Generic value parsing: tokens to value trees, colors, math functions
 * (calc and friends) with type checking and simplification, and the
 * packing of trees into reference-counted blobs.
 *
 * Property-specific validation happens afterwards (propparse.c) on the
 * generic tree. Recursion here follows the nesting of functions in one
 * value, which is limited to CSS_MAX_DEPTH. */
#include "css_int.h"

#include <math.h>

#define CSS_MAX_DEPTH 32

static int parse_depth;

/* ---- Node construction ---- */

AuCssValue *css_new_value(CssArena *a, int type)
{
    AuCssValue *v = css_arena_alloc(a, sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = (uint8_t)type;
    return v;
}

AuCssValue *css_new_list(CssArena *a, int type, uint32_t n)
{
    AuCssValue *v = css_new_value(a, type);
    v->n = n;
    v->u.items = n ? css_arena_alloc(a, n * sizeof(AuCssValue)) : NULL;
    if (n)
        memset(v->u.items, 0, n * sizeof(AuCssValue));
    return v;
}

AuCssValue *css_new_kw(CssArena *a, uint16_t kw)
{
    AuCssValue *v = css_new_value(a, AU_CSSV_KEYWORD);
    v->kw = kw;
    return v;
}

AuCssValue *css_new_num(CssArena *a, int type, float num, int unit)
{
    AuCssValue *v = css_new_value(a, type);
    v->u.num = num;
    v->unit = (uint8_t)unit;
    return v;
}

const AuCssValue *css_items(const AuCssValue *v, uint32_t *n)
{
    if (v && (v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST)) {
        *n = v->n;
        return v->u.items;
    }
    *n = v ? 1 : 0;
    return v;
}

static bool is_string_type(int t) { return t == AU_CSSV_IDENT || t == AU_CSSV_STRING || t == AU_CSSV_URL; }
static bool is_tree_type(int t)
{
    return t == AU_CSSV_FUNCTION || t == AU_CSSV_CALC || t == AU_CSSV_LIST || t == AU_CSSV_COMMA_LIST;
}

/* ---- Units ---- */

bool css_unit_canonical(int unit, float *factor, int *canon)
{
    switch (unit) {
    case AU_UNIT_PX: *factor = 1; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_CM: *factor = 96.0f / 2.54f; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_MM: *factor = 96.0f / 25.4f; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_Q: *factor = 96.0f / 101.6f; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_IN: *factor = 96; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_PT: *factor = 96.0f / 72; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_PC: *factor = 16; *canon = AU_UNIT_PX; return true;
    case AU_UNIT_DEG: *factor = 1; *canon = AU_UNIT_DEG; return true;
    case AU_UNIT_GRAD: *factor = 0.9f; *canon = AU_UNIT_DEG; return true;
    case AU_UNIT_RAD: *factor = (float)(180.0 / 3.14159265358979323846); *canon = AU_UNIT_DEG; return true;
    case AU_UNIT_TURN: *factor = 360; *canon = AU_UNIT_DEG; return true;
    case AU_UNIT_S: *factor = 1; *canon = AU_UNIT_S; return true;
    case AU_UNIT_MS: *factor = 0.001f; *canon = AU_UNIT_S; return true;
    case AU_UNIT_HZ: *factor = 1; *canon = AU_UNIT_HZ; return true;
    case AU_UNIT_KHZ: *factor = 1000; *canon = AU_UNIT_HZ; return true;
    case AU_UNIT_DPPX: *factor = 1; *canon = AU_UNIT_DPPX; return true;
    case AU_UNIT_X: *factor = 1; *canon = AU_UNIT_DPPX; return true;
    case AU_UNIT_DPI: *factor = 1.0f / 96; *canon = AU_UNIT_DPPX; return true;
    case AU_UNIT_DPCM: *factor = 2.54f / 96; *canon = AU_UNIT_DPPX; return true;
    case AU_UNIT_FR: *factor = 1; *canon = AU_UNIT_FR; return true;
    default: return false;
    }
}

static unsigned unit_type(int unit)
{
    if (au_unit_is_length(unit))
        return CT_LENGTH;
    switch (unit) {
    case AU_UNIT_DEG: case AU_UNIT_GRAD: case AU_UNIT_RAD: case AU_UNIT_TURN: return CT_ANGLE;
    case AU_UNIT_S: case AU_UNIT_MS: return CT_TIME;
    case AU_UNIT_HZ: case AU_UNIT_KHZ: return CT_FREQ;
    case AU_UNIT_DPI: case AU_UNIT_DPCM: case AU_UNIT_DPPX: case AU_UNIT_X: return CT_RES;
    case AU_UNIT_FR: return CT_FLEX;
    default: return CT_INVALID;
    }
}

/* ---- Colors ---- */

static const struct {
    const char *name;
    uint32_t rgb;
} named_colors[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082}, {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db},
    {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970},
    {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead},
    {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa},
    {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb},
    {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3},
    {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

static AuColor norm_color(AuColor c) { return c == AU_COLOR_CURRENT ? AU_COLOR_TRANSPARENT : c; }

bool css_color_from_name(const char *s, size_t n, AuColor *out)
{
    char buf[24];
    if (!css_lower(s, n, buf, sizeof buf))
        return false;
    size_t lo = 0, hi = CSS_ARRAY_LEN(named_colors);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(buf, named_colors[mid].name);
        if (c == 0) {
            *out = (named_colors[mid].rgb << 8) | 0xFF;
            return true;
        }
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return false;
}

static int hexv(int c) { return au_is_digit(c) ? c - '0' : au_lower(c) - 'a' + 10; }

bool css_color_from_hex(const char *s, size_t n, AuColor *out)
{
    for (size_t i = 0; i < n; i++)
        if (!au_is_hex((unsigned char)s[i]))
            return false;
    uint32_t r, g, b, a = 255;
    switch (n) {
    case 3:
    case 4:
        r = (uint32_t)hexv(s[0]) * 17;
        g = (uint32_t)hexv(s[1]) * 17;
        b = (uint32_t)hexv(s[2]) * 17;
        if (n == 4)
            a = (uint32_t)hexv(s[3]) * 17;
        break;
    case 6:
    case 8:
        r = (uint32_t)(hexv(s[0]) * 16 + hexv(s[1]));
        g = (uint32_t)(hexv(s[2]) * 16 + hexv(s[3]));
        b = (uint32_t)(hexv(s[4]) * 16 + hexv(s[5]));
        if (n == 8)
            a = (uint32_t)(hexv(s[6]) * 16 + hexv(s[7]));
        break;
    default:
        return false;
    }
    *out = norm_color(AU_COLOR_RGBA(r, g, b, a));
    return true;
}

bool css_color_keyword(uint16_t kw, AuColor *out, bool *is_current)
{
    uint32_t rgb;
    *is_current = false;
    switch (kw) {
    case AU_K_transparent: *out = AU_COLOR_TRANSPARENT; return true;
    case AU_K_currentcolor: *out = AU_COLOR_CURRENT; *is_current = true; return true;
    case AU_K_canvas: case AU_K_field: case AU_K_window: case AU_K_infobackground: rgb = 0xffffff; break;
    case AU_K_canvastext: case AU_K_fieldtext: case AU_K_buttontext: case AU_K_windowtext: case AU_K_menutext:
    case AU_K_captiontext: case AU_K_infotext: case AU_K_marktext: case AU_K_inactivecaptiontext:
        rgb = 0x000000; break;
    case AU_K_linktext: case AU_K__webkit_link: rgb = 0x0000ee; break;
    case AU_K_visitedtext: rgb = 0x551a8b; break;
    case AU_K_activetext: rgb = 0xff0000; break;
    case AU_K_buttonface: case AU_K_threedface: case AU_K_threedlightshadow: case AU_K_menu:
    case AU_K_scrollbar: case AU_K_activecaption: case AU_K_inactivecaption: case AU_K_appworkspace:
    case AU_K_background: case AU_K_buttonhighlight: case AU_K_threedhighlight:
        rgb = 0xefefef; break;
    case AU_K_buttonborder: case AU_K_activeborder: case AU_K_inactiveborder: case AU_K_buttonshadow:
    case AU_K_threedshadow: case AU_K_windowframe: case AU_K_threeddarkshadow:
        rgb = 0x767676; break;
    case AU_K_highlight: case AU_K_selecteditem: rgb = 0x3399ff; break;
    case AU_K_highlighttext: case AU_K_selecteditemtext: case AU_K_accentcolortext: rgb = 0xffffff; break;
    case AU_K_mark: rgb = 0xffff00; break;
    case AU_K_graytext: rgb = 0x808080; break;
    case AU_K_accentcolor: rgb = 0x0075ff; break;
    case AU_K__webkit_focus_ring_color: rgb = 0x101010; break;
    default: return false;
    }
    *out = (rgb << 8) | 0xFF;
    return true;
}

/* NaN (calc(0/0) in a colour) gives 0: it fails both comparisons. */
static float clamp01(float v) { return v > 0 ? (v > 1 ? 1 : v) : 0; }
/* Rounds half up; the small bias absorbs float error (hsl(180 100% 25%)
 * is 127.5, which must give 128). */
static uint32_t to8(float v) { return (uint32_t)(clamp01(v) * 255.0f + 0.5f + 1e-4f); }

static AuColor rgbf(float r, float g, float b, float a)
{
    return norm_color(AU_COLOR_RGBA(to8(r), to8(g), to8(b), to8(a)));
}

static float hue2rgb(float m1, float m2, float h)
{
    if (h < 0)
        h += 1;
    if (h > 1)
        h -= 1;
    if (h * 6 < 1)
        return m1 + (m2 - m1) * h * 6;
    if (h * 2 < 1)
        return m2;
    if (h * 3 < 2)
        return m1 + (m2 - m1) * (2.0f / 3 - h) * 6;
    return m1;
}

static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b)
{
    h = fmodf(h, 360);
    if (h < 0)
        h += 360;
    h /= 360;
    s = clamp01(s);
    l = clamp01(l);
    float m2 = l <= 0.5f ? l * (s + 1) : l + s - l * s;
    float m1 = l * 2 - m2;
    *r = hue2rgb(m1, m2, h + 1.0f / 3);
    *g = hue2rgb(m1, m2, h);
    *b = hue2rgb(m1, m2, h - 1.0f / 3);
}

static float lin_to_srgb(float c)
{
    float a = fabsf(c);
    float v = a <= 0.0031308f ? 12.92f * a : 1.055f * powf(a, 1 / 2.4f) - 0.055f;
    return c < 0 ? -v : v;
}

static float srgb_to_lin(float c)
{
    float a = fabsf(c);
    float v = a <= 0.04045f ? a / 12.92f : powf((a + 0.055f) / 1.055f, 2.4f);
    return c < 0 ? -v : v;
}

static void xyz65_to_srgb(float x, float y, float z, float *r, float *g, float *b)
{
    *r = lin_to_srgb(3.2409699f * x - 1.5373832f * y - 0.4986108f * z);
    *g = lin_to_srgb(-0.9692436f * x + 1.8759675f * y + 0.0415551f * z);
    *b = lin_to_srgb(0.0556301f * x - 0.2039770f * y + 1.0569715f * z);
}

static void xyz50_to_65(float *x, float *y, float *z)
{
    float X = *x, Y = *y, Z = *z;
    *x = 0.9554734f * X - 0.0230985f * Y + 0.0632593f * Z;
    *y = -0.0283697f * X + 1.0099956f * Y + 0.0210414f * Z;
    *z = 0.0123140f * X - 0.0205077f * Y + 1.3303659f * Z;
}

static void lab_to_srgb(float L, float a, float bb, float *r, float *g, float *b)
{
    const float k = 24389.0f / 27, e = 216.0f / 24389;
    float f1 = (L + 16) / 116, f0 = a / 500 + f1, f2 = f1 - bb / 200;
    float x = f0 * f0 * f0 > e ? f0 * f0 * f0 : (116 * f0 - 16) / k;
    float y = L > k * e ? powf((L + 16) / 116, 3) : L / k;
    float z = f2 * f2 * f2 > e ? f2 * f2 * f2 : (116 * f2 - 16) / k;
    x *= 0.3457f / 0.3585f;
    z *= (1 - 0.3457f - 0.3585f) / 0.3585f;
    xyz50_to_65(&x, &y, &z);
    xyz65_to_srgb(x, y, z, r, g, b);
}

static void oklab_to_srgb(float L, float a, float bb, float *r, float *g, float *b)
{
    float l = L + 0.3963377774f * a + 0.2158037573f * bb;
    float m = L - 0.1055613458f * a - 0.0638541728f * bb;
    float s = L - 0.0894841775f * a - 1.2914855480f * bb;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    *r = lin_to_srgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    *g = lin_to_srgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    *b = lin_to_srgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

static void srgb_to_oklab(float r, float g, float b, float *L, float *A, float *B)
{
    r = srgb_to_lin(r);
    g = srgb_to_lin(g);
    b = srgb_to_lin(b);
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    *L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    *A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    *B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

/* A color component: number, percentage (scaled by pct_scale), angle (deg)
 * or none. Returns false if absent or of a wrong type. */
typedef struct {
    float v;
    bool is_pct, is_none, is_angle;
} Comp;

static bool get_comp(const AuCssValue *v, Comp *c)
{
    memset(c, 0, sizeof *c);
    if (!v)
        return false;
    if (v->type == AU_CSSV_NUMBER) {
        c->v = v->u.num;
        return true;
    }
    if (v->type == AU_CSSV_PERCENT) {
        c->v = v->u.num;
        c->is_pct = true;
        return true;
    }
    if (v->type == AU_CSSV_KEYWORD && v->kw == AU_K_none) {
        c->is_none = true;
        return true;
    }
    if (v->type == AU_CSSV_DIMENSION) {
        float f;
        int canon;
        if (css_unit_canonical(v->unit, &f, &canon) && canon == AU_UNIT_DEG) {
            c->v = v->u.num * f;
            c->is_angle = true;
            return true;
        }
    }
    return false;
}

/* Splits a color function's arguments into up to 4 components (alpha
 * after '/' or as a 4th comma argument). Returns the count of color
 * components (3) or -1. legacy is set for comma syntax. */
static int color_args(const AuCssValue *fn, const AuCssValue **c, bool *legacy, bool allow_legacy)
{
    c[0] = c[1] = c[2] = c[3] = NULL;
    *legacy = false;
    if (fn->n == 1) {
        /* space syntax: one LIST argument */
        uint32_t n;
        const AuCssValue *it = css_items(&fn->u.items[0], &n);
        int k = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (it[i].type == AU_CSSV_DELIM && it[i].unit == '/') {
                if (k != 3 || i + 2 != n)
                    return -1;
                c[3] = &it[i + 1];
                return 3;
            }
            if (k >= 3)
                return -1;
            c[k++] = &it[i];
        }
        return k == 3 ? 3 : -1;
    }
    if (!allow_legacy || (fn->n != 3 && fn->n != 4))
        return -1;
    for (uint32_t i = 0; i < fn->n; i++) {
        if (is_tree_type(fn->u.items[i].type) && fn->u.items[i].type != AU_CSSV_CALC)
            return -1;
        c[i] = &fn->u.items[i];
    }
    *legacy = true;
    return 3;
}

static bool alpha_of(const AuCssValue *v, float *a)
{
    Comp c;
    if (!v) {
        *a = 1;
        return true;
    }
    if (!get_comp(v, &c) || c.is_angle)
        return false;
    *a = c.is_none ? 0 : c.is_pct ? c.v / 100 : c.v;
    return true;
}

static bool parse_color_fn(const AuCssValue *fn, AuColor *out);
bool css_value_to_color(const AuCssValue *v, AuColor *out, bool *current);

/* Converts a value that denotes a color (COLOR node, color keyword, named
 * color identifier) to RGBA. currentcolor is reported. */
bool css_value_to_color(const AuCssValue *v, AuColor *out, bool *current)
{
    *current = false;
    if (!v)
        return false;
    if (v->type == AU_CSSV_COLOR) {
        *out = v->u.rgba;
        *current = v->u.rgba == AU_COLOR_CURRENT;
        return true;
    }
    if (v->type == AU_CSSV_KEYWORD)
        return css_color_keyword(v->kw, out, current);
    if (v->type == AU_CSSV_IDENT)
        return css_color_from_name(v->u.str, v->n, out);
    return false;
}

static bool parse_rgb(const AuCssValue *fn, AuColor *out)
{
    const AuCssValue *cv[4];
    bool legacy;
    if (color_args(fn, cv, &legacy, true) != 3)
        return false;
    Comp c[3];
    for (int i = 0; i < 3; i++)
        if (!get_comp(cv[i], &c[i]) || c[i].is_angle || (legacy && c[i].is_none))
            return false;
    if (legacy && (c[0].is_pct != c[1].is_pct || c[0].is_pct != c[2].is_pct))
        return false;
    float a;
    if (!alpha_of(cv[3], &a))
        return false;
    float f[3];
    for (int i = 0; i < 3; i++)
        f[i] = c[i].is_none ? 0 : c[i].is_pct ? c[i].v / 100 : c[i].v / 255;
    *out = rgbf(f[0], f[1], f[2], a);
    return true;
}

static bool parse_hsl_hwb(const AuCssValue *fn, AuColor *out, bool hwb)
{
    const AuCssValue *cv[4];
    bool legacy;
    if (color_args(fn, cv, &legacy, !hwb) != 3)
        return false;
    Comp c[3];
    for (int i = 0; i < 3; i++)
        if (!get_comp(cv[i], &c[i]) || (legacy && c[i].is_none))
            return false;
    if (c[0].is_pct || c[1].is_angle || c[2].is_angle)
        return false;
    if (legacy && (!c[1].is_pct || !c[2].is_pct))
        return false;
    float a;
    if (!alpha_of(cv[3], &a))
        return false;
    float h = c[0].is_none ? 0 : c[0].v;
    float s = c[1].is_none ? 0 : c[1].v / 100, l = c[2].is_none ? 0 : c[2].v / 100;
    float r, g, b;
    if (hwb) {
        float w = s, bl = l;
        if (w + bl >= 1) {
            float gray = w / (w + bl);
            r = g = b = gray;
        } else {
            hsl_to_rgb(h, 1, 0.5f, &r, &g, &b);
            r = r * (1 - w - bl) + w;
            g = g * (1 - w - bl) + w;
            b = b * (1 - w - bl) + w;
        }
    } else {
        hsl_to_rgb(h, s, l, &r, &g, &b);
    }
    *out = rgbf(r, g, b, a);
    return true;
}

static bool parse_lab(const AuCssValue *fn, AuColor *out, int kind)
{
    const AuCssValue *cv[4];
    bool legacy;
    if (color_args(fn, cv, &legacy, false) != 3)
        return false;
    Comp c[3];
    for (int i = 0; i < 3; i++)
        if (!get_comp(cv[i], &c[i]))
            return false;
    float a;
    if (!alpha_of(cv[3], &a))
        return false;
    bool ok = kind == AU_K_oklab || kind == AU_K_oklch;
    bool polar = kind == AU_K_lch || kind == AU_K_oklch;
    float Lscale = ok ? 1.0f / 100 : 1; /* percent of L */
    float L = c[0].is_none ? 0 : c[0].is_pct ? c[0].v * (ok ? 0.01f : 1) : c[0].v;
    (void)Lscale;
    float A, B;
    if (polar) {
        float C = c[1].is_none ? 0 : c[1].is_pct ? c[1].v * (ok ? 0.004f : 1.5f) : c[1].v;
        float H = c[2].is_none ? 0 : c[2].v;
        if (c[2].is_pct)
            return false;
        A = C * cosf(H * 3.14159265f / 180);
        B = C * sinf(H * 3.14159265f / 180);
    } else {
        float s = ok ? 0.004f : 1.25f;
        A = c[1].is_none ? 0 : c[1].is_pct ? c[1].v * s : c[1].v;
        B = c[2].is_none ? 0 : c[2].is_pct ? c[2].v * s : c[2].v;
    }
    float r, g, b;
    if (ok)
        oklab_to_srgb(L, A, B, &r, &g, &b);
    else
        lab_to_srgb(L, A, B, &r, &g, &b);
    *out = rgbf(r, g, b, a);
    return true;
}

/* color(space c1 c2 c3 [/ a]). srgb, srgb-linear, display-p3, xyz,
 * xyz-d50 and xyz-d65 convert exactly (then clamp); a98-rgb, prophoto-rgb
 * and rec2020 are treated as sRGB coordinates. */
static bool parse_color_space_fn(const AuCssValue *fn, AuColor *out)
{
    if (fn->n != 1)
        return false;
    uint32_t n;
    const AuCssValue *it = css_items(&fn->u.items[0], &n);
    if (n < 4 || it[0].type != AU_CSSV_KEYWORD)
        return false;
    int space = it[0].kw;
    float c[3] = {0, 0, 0}, a = 1;
    uint32_t k = 0, i;
    for (i = 1; i < n; i++) {
        if (it[i].type == AU_CSSV_DELIM && it[i].unit == '/') {
            if (i + 2 != n || !alpha_of(&it[i + 1], &a))
                return false;
            break;
        }
        Comp cc;
        if (k >= 3 || !get_comp(&it[i], &cc) || cc.is_angle)
            return false;
        c[k++] = cc.is_none ? 0 : cc.is_pct ? cc.v / 100 : cc.v;
    }
    if (k != 3)
        return false;
    float r, g, b;
    switch (space) {
    case AU_K_srgb: case AU_K_a98_rgb: case AU_K_prophoto_rgb: case AU_K_rec2020:
        r = c[0]; g = c[1]; b = c[2];
        break;
    case AU_K_srgb_linear:
        r = lin_to_srgb(c[0]); g = lin_to_srgb(c[1]); b = lin_to_srgb(c[2]);
        break;
    case AU_K_display_p3: {
        float lr = srgb_to_lin(c[0]), lg = srgb_to_lin(c[1]), lb = srgb_to_lin(c[2]);
        float x = 0.4865709f * lr + 0.2656677f * lg + 0.1982173f * lb;
        float y = 0.2289746f * lr + 0.6917385f * lg + 0.0792869f * lb;
        float z = 0.0000000f * lr + 0.0451134f * lg + 1.0439444f * lb;
        xyz65_to_srgb(x, y, z, &r, &g, &b);
        break;
    }
    case AU_K_xyz: case AU_K_xyz_d65:
        xyz65_to_srgb(c[0], c[1], c[2], &r, &g, &b);
        break;
    case AU_K_xyz_d50: {
        float x = c[0], y = c[1], z = c[2];
        xyz50_to_65(&x, &y, &z);
        xyz65_to_srgb(x, y, z, &r, &g, &b);
        break;
    }
    default:
        return false;
    }
    *out = rgbf(r, g, b, a);
    return true;
}

/* color-mix(in <space>, c1 [p1], c2 [p2]). Mixed in sRGB, or in OKLab for
 * oklab/oklch; other spaces mix in sRGB. currentcolor is not supported. */
static bool parse_color_mix(const AuCssValue *fn, AuColor *out)
{
    if (fn->n != 3)
        return false;
    uint32_t n;
    const AuCssValue *sp = css_items(&fn->u.items[0], &n);
    if (n < 2 || !css_is_kw(&sp[0], AU_K_in) || sp[1].type != AU_CSSV_KEYWORD)
        return false;
    bool oklab = sp[1].kw == AU_K_oklab || sp[1].kw == AU_K_oklch;
    AuColor col[2];
    float pct[2] = {-1, -1};
    for (int k = 0; k < 2; k++) {
        const AuCssValue *it = css_items(&fn->u.items[1 + k], &n);
        bool cur, got = false;
        for (uint32_t i = 0; i < n; i++) {
            if (it[i].type == AU_CSSV_PERCENT && pct[k] < 0)
                pct[k] = it[i].u.num;
            else if (!got && css_value_to_color(&it[i], &col[k], &cur) && !cur)
                got = true;
            else
                return false;
        }
        if (!got)
            return false;
    }
    float p1 = pct[0], p2 = pct[1];
    if (p1 < 0 && p2 < 0)
        p1 = p2 = 50;
    else if (p1 < 0)
        p1 = 100 - p2;
    else if (p2 < 0)
        p2 = 100 - p1;
    float sum = p1 + p2;
    if (sum <= 0)
        return false;
    float mult = sum < 100 ? sum / 100 : 1;
    float w2 = p2 / sum;
    float c1[4], c2[4];
    for (int k = 0; k < 4; k++) {
        c1[k] = (float)((col[0] >> (24 - 8 * k)) & 0xFF) / 255;
        c2[k] = (float)((col[1] >> (24 - 8 * k)) & 0xFF) / 255;
    }
    if (oklab) {
        srgb_to_oklab(c1[0], c1[1], c1[2], &c1[0], &c1[1], &c1[2]);
        srgb_to_oklab(c2[0], c2[1], c2[2], &c2[0], &c2[1], &c2[2]);
    }
    /* premultiplied interpolation */
    float a = c1[3] * (1 - w2) + c2[3] * w2;
    float m[3];
    for (int k = 0; k < 3; k++) {
        float v = c1[k] * c1[3] * (1 - w2) + c2[k] * c2[3] * w2;
        m[k] = a > 0 ? v / a : 0;
    }
    if (oklab)
        oklab_to_srgb(m[0], m[1], m[2], &m[0], &m[1], &m[2]);
    *out = rgbf(m[0], m[1], m[2], a * mult);
    return true;
}

static bool parse_color_fn(const AuCssValue *fn, AuColor *out)
{
    switch (fn->kw) {
    case AU_K_rgb:
    case AU_K_rgba:
        return parse_rgb(fn, out);
    case AU_K_hsl:
    case AU_K_hsla:
        return parse_hsl_hwb(fn, out, false);
    case AU_K_hwb:
        return parse_hsl_hwb(fn, out, true);
    case AU_K_lab:
    case AU_K_lch:
    case AU_K_oklab:
    case AU_K_oklch:
        return parse_lab(fn, out, fn->kw);
    case AU_K_color:
        return parse_color_space_fn(fn, out);
    case AU_K_color_mix:
        return parse_color_mix(fn, out);
    case AU_K_light_dark: {
        /* color-scheme is not implemented, so the used scheme is light */
        bool cur;
        return fn->n == 2 && css_value_to_color(&fn->u.items[0], out, &cur) && !cur;
    }
    default:
        return false;
    }
}

static bool is_color_fn(int kw)
{
    switch (kw) {
    case AU_K_rgb: case AU_K_rgba: case AU_K_hsl: case AU_K_hsla: case AU_K_hwb: case AU_K_lab:
    case AU_K_lch: case AU_K_oklab: case AU_K_oklch: case AU_K_color: case AU_K_color_mix:
    case AU_K_light_dark:
        return true;
    default:
        return false;
    }
}

/* ---- Math functions ---- */

static bool is_math_fn(int kw)
{
    switch (kw) {
    case AU_K_calc: case AU_K__webkit_calc: case AU_K_min: case AU_K_max: case AU_K_clamp: case AU_K_round:
    case AU_K_mod: case AU_K_rem_fn: case AU_K_abs: case AU_K_sign: case AU_K_sin: case AU_K_cos: case AU_K_tan:
    case AU_K_asin: case AU_K_acos: case AU_K_atan: case AU_K_atan2: case AU_K_pow: case AU_K_sqrt:
    case AU_K_hypot: case AU_K_log: case AU_K_exp:
        return true;
    default:
        return false;
    }
}

typedef struct {
    CssParseCtx *cx;
    const CssTokens *tk;
} MathP;

static AuCssValue *math_sum(MathP *m, uint32_t *i, uint32_t b);
static AuCssValue *math_fn(MathP *m, uint32_t fi);

static void skip_ws(const CssTokens *tk, uint32_t *i, uint32_t b)
{
    while (*i < b && tk->t[*i].type == TK_WS)
        (*i)++;
}

static AuCssValue *calc_node(CssArena *a, int op, uint32_t n)
{
    AuCssValue *v = css_new_list(a, AU_CSSV_CALC, n);
    v->unit = (uint8_t)op;
    return v;
}

static AuCssValue *math_value(MathP *m, uint32_t *i, uint32_t b)
{
    skip_ws(m->tk, i, b);
    if (*i >= b)
        return NULL;
    const CssToken *t = &m->tk->t[*i];
    CssArena *a = m->cx->arena;
    switch (t->type) {
    case TK_NUMBER:
        (*i)++;
        return css_new_num(a, AU_CSSV_NUMBER, t->num, 0);
    case TK_PERCENT:
        (*i)++;
        return css_new_num(a, AU_CSSV_PERCENT, t->num, 0);
    case TK_DIMENSION:
        if (t->unit == AU_UNIT_UNKNOWN)
            return NULL;
        (*i)++;
        return css_new_num(a, AU_CSSV_DIMENSION, t->num, t->unit);
    case TK_IDENT: {
        float v;
        switch (t->kw) {
        case AU_K_e: v = 2.718281828f; break;
        case AU_K_pi: v = 3.14159265f; break;
        case AU_K_infinity: v = INFINITY; break;
        case AU_K__infinity: v = -INFINITY; break;
        case AU_K_nan: v = NAN; break;
        default: return NULL;
        }
        (*i)++;
        return css_new_num(a, AU_CSSV_NUMBER, v, 0);
    }
    case TK_LPAREN: {
        uint32_t end = t->len, j = *i + 1;
        if (end > b || m->tk->t[end - 1].type != TK_RPAREN)
            return NULL;
        AuCssValue *v = math_sum(m, &j, end - 1);
        skip_ws(m->tk, &j, end - 1);
        if (!v || j != end - 1)
            return NULL;
        *i = end;
        return v;
    }
    case TK_FUNCTION: {
        if (!is_math_fn(t->kw) || t->len > b)
            return NULL;
        AuCssValue *v = math_fn(m, *i);
        if (v)
            *i = t->len;
        return v;
    }
    default:
        return NULL;
    }
}

static AuCssValue *math_product(MathP *m, uint32_t *i, uint32_t b)
{
    AuCssValue *first = math_value(m, i, b);
    if (!first)
        return NULL;
    AuCssValue *items[64];
    uint32_t n = 0;
    items[n++] = first;
    for (;;) {
        uint32_t j = *i;
        skip_ws(m->tk, &j, b);
        if (j >= b || m->tk->t[j].type != TK_DELIM || (m->tk->t[j].num != '*' && m->tk->t[j].num != '/'))
            break;
        bool div = m->tk->t[j].num == '/';
        j++;
        AuCssValue *v = math_value(m, &j, b);
        if (!v || n >= 64)
            return NULL;
        if (div) {
            AuCssValue *inv = calc_node(m->cx->arena, AU_CALC_INVERT, 1);
            inv->u.items[0] = *v;
            v = inv;
        }
        items[n++] = v;
        *i = j;
    }
    if (n == 1)
        return first;
    AuCssValue *p = calc_node(m->cx->arena, AU_CALC_PRODUCT, n);
    for (uint32_t k = 0; k < n; k++)
        p->u.items[k] = *items[k];
    return p;
}

static AuCssValue *math_sum(MathP *m, uint32_t *i, uint32_t b)
{
    AuCssValue *first = math_product(m, i, b);
    if (!first)
        return NULL;
    AuCssValue *items[64];
    uint32_t n = 0;
    items[n++] = first;
    for (;;) {
        uint32_t j = *i, k = *i;
        skip_ws(m->tk, &k, b);
        if (k >= b || m->tk->t[k].type == TK_COMMA)
            break; /* the end of this argument */
        if (m->tk->t[j].type != TK_WS)
            return NULL; /* operators need whitespace around them */
        j = k;
        const CssToken *t = &m->tk->t[j];
        if (t->type != TK_DELIM || (t->num != '+' && t->num != '-'))
            return NULL;
        bool neg = t->num == '-';
        j++;
        if (j >= b || m->tk->t[j].type != TK_WS)
            return NULL;
        AuCssValue *v = math_product(m, &j, b);
        if (!v || n >= 64)
            return NULL;
        if (neg) {
            AuCssValue *ng = calc_node(m->cx->arena, AU_CALC_NEGATE, 1);
            ng->u.items[0] = *v;
            v = ng;
        }
        items[n++] = v;
        *i = j;
    }
    if (n == 1)
        return first;
    AuCssValue *s = calc_node(m->cx->arena, AU_CALC_SUM, n);
    for (uint32_t k = 0; k < n; k++)
        s->u.items[k] = *items[k];
    return s;
}

/* Parses the comma-separated calc-sum arguments of a math function. */
static int math_args(MathP *m, uint32_t fi, AuCssValue **args, int max, bool allow_kw_first, uint16_t *kw)
{
    const CssToken *f = &m->tk->t[fi];
    uint32_t end = f->len;
    uint32_t b = (end > fi + 1 && m->tk->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
    uint32_t i = fi + 1;
    int n = 0;
    if (kw)
        *kw = 0;
    for (;;) {
        skip_ws(m->tk, &i, b);
        if (n == 0 && allow_kw_first && i < b && m->tk->t[i].type == TK_IDENT && m->tk->t[i].kw &&
            (m->tk->t[i].kw == AU_K_nearest || m->tk->t[i].kw == AU_K_up || m->tk->t[i].kw == AU_K_down ||
             m->tk->t[i].kw == AU_K_to_zero || m->tk->t[i].kw == AU_K_none)) {
            *kw = m->tk->t[i].kw;
            i++;
            skip_ws(m->tk, &i, b);
            if (i < b && m->tk->t[i].type == TK_COMMA && *kw != AU_K_none) {
                i++;
                continue;
            }
            if (*kw == AU_K_none) {
                /* clamp(none, ...) */
                args[n++] = css_new_kw(m->cx->arena, AU_K_none);
                *kw = 0;
                if (i < b && m->tk->t[i].type == TK_COMMA) {
                    i++;
                    continue;
                }
                return i == b ? n : -1;
            }
            return -1;
        }
        if (n >= max)
            return -1;
        AuCssValue *v;
        skip_ws(m->tk, &i, b);
        if (i < b && m->tk->t[i].type == TK_IDENT && m->tk->t[i].kw == AU_K_none && allow_kw_first) {
            v = css_new_kw(m->cx->arena, AU_K_none);
            i++;
        } else {
            v = math_sum(m, &i, b);
        }
        if (!v)
            return -1;
        args[n++] = v;
        skip_ws(m->tk, &i, b);
        if (i == b)
            return n;
        if (m->tk->t[i].type != TK_COMMA)
            return -1;
        i++;
    }
}

static AuCssValue *math_fn(MathP *m, uint32_t fi)
{
    if (++parse_depth > CSS_MAX_DEPTH) {
        parse_depth--;
        return NULL;
    }
    const CssToken *f = &m->tk->t[fi];
    AuCssValue *args[32];
    AuCssValue *res = NULL;
    uint16_t kw = 0;
    int n, op = -1;
    CssArena *a = m->cx->arena;
    switch (f->kw) {
    case AU_K_calc:
    case AU_K__webkit_calc:
        n = math_args(m, fi, args, 1, false, NULL);
        if (n == 1)
            res = args[0];
        break;
    case AU_K_min: op = AU_CALC_MIN; goto many;
    case AU_K_max: op = AU_CALC_MAX; goto many;
    case AU_K_hypot: op = AU_CALC_HYPOT;
    many:
        n = math_args(m, fi, args, 32, false, NULL);
        if (n >= 1) {
            res = calc_node(a, op, (uint32_t)n);
            for (int k = 0; k < n; k++)
                res->u.items[k] = *args[k];
        }
        break;
    case AU_K_clamp:
        n = math_args(m, fi, args, 3, true, NULL);
        if (n == 3 && args[1]->type != AU_CSSV_KEYWORD) {
            res = calc_node(a, AU_CALC_CLAMP, 3);
            for (int k = 0; k < 3; k++)
                res->u.items[k] = *args[k];
        }
        break;
    case AU_K_round:
        n = math_args(m, fi, args, 2, true, &kw);
        if (n >= 1 && args[0]->type != AU_CSSV_KEYWORD && (n == 1 || args[1]->type != AU_CSSV_KEYWORD)) {
            op = kw == AU_K_up ? AU_CALC_ROUND_UP : kw == AU_K_down ? AU_CALC_ROUND_DOWN
                 : kw == AU_K_to_zero ? AU_CALC_ROUND_TO_ZERO : AU_CALC_ROUND_NEAREST;
            res = calc_node(a, op, 2);
            res->u.items[0] = *args[0];
            if (n == 2)
                res->u.items[1] = *args[1];
            else
                res->u.items[1] = *css_new_num(a, AU_CSSV_NUMBER, 1, 0);
        }
        break;
    case AU_K_mod: op = AU_CALC_MOD; goto two;
    case AU_K_rem_fn: op = AU_CALC_REM; goto two;
    case AU_K_atan2: op = AU_CALC_ATAN2; goto two;
    case AU_K_pow: op = AU_CALC_POW;
    two:
        n = math_args(m, fi, args, 2, false, NULL);
        if (n == 2) {
            res = calc_node(a, op, 2);
            res->u.items[0] = *args[0];
            res->u.items[1] = *args[1];
        }
        break;
    case AU_K_log:
        n = math_args(m, fi, args, 2, false, NULL);
        if (n >= 1) {
            res = calc_node(a, AU_CALC_LOG, (uint32_t)n);
            for (int k = 0; k < n; k++)
                res->u.items[k] = *args[k];
        }
        break;
    case AU_K_abs: op = AU_CALC_ABS; goto one;
    case AU_K_sign: op = AU_CALC_SIGN; goto one;
    case AU_K_sin: op = AU_CALC_SIN; goto one;
    case AU_K_cos: op = AU_CALC_COS; goto one;
    case AU_K_tan: op = AU_CALC_TAN; goto one;
    case AU_K_asin: op = AU_CALC_ASIN; goto one;
    case AU_K_acos: op = AU_CALC_ACOS; goto one;
    case AU_K_atan: op = AU_CALC_ATAN; goto one;
    case AU_K_sqrt: op = AU_CALC_SQRT; goto one;
    case AU_K_exp: op = AU_CALC_EXP;
    one:
        n = math_args(m, fi, args, 1, false, NULL);
        if (n == 1) {
            res = calc_node(a, op, 1);
            res->u.items[0] = *args[0];
        }
        break;
    default:
        break;
    }
    parse_depth--;
    return res;
}

/* Type of a value tree per css-values-4 (simplified to one base type plus
 * a percent hint). */
unsigned css_value_type(const AuCssValue *v)
{
    switch (v->type) {
    case AU_CSSV_NUMBER:
        return CT_NUMBER;
    case AU_CSSV_PERCENT:
        return CT_PERCENT;
    case AU_CSSV_DIMENSION:
        return unit_type(v->unit);
    case AU_CSSV_KEYWORD:
        return v->kw == AU_K_none ? CT_NUMBER : CT_INVALID;
    case AU_CSSV_CALC:
        break;
    default:
        return CT_INVALID;
    }
    unsigned t = 0;
    switch (v->unit) {
    case AU_CALC_SUM:
    case AU_CALC_MIN:
    case AU_CALC_MAX:
    case AU_CALC_CLAMP:
    case AU_CALC_HYPOT:
    case AU_CALC_ROUND_NEAREST:
    case AU_CALC_ROUND_UP:
    case AU_CALC_ROUND_DOWN:
    case AU_CALC_ROUND_TO_ZERO:
    case AU_CALC_MOD:
    case AU_CALC_REM: {
        bool first = true;
        for (uint32_t i = 0; i < v->n; i++) {
            if (css_is_kw(&v->u.items[i], AU_K_none))
                continue;
            unsigned u = css_value_type(&v->u.items[i]);
            if (u & CT_INVALID)
                return CT_INVALID;
            if (first) {
                t = u;
                first = false;
            } else if (u != t) {
                unsigned m = t | u;
                if ((m & ~(unsigned)(CT_LENGTH | CT_PERCENT)) == 0 && t && u)
                    t = m;
                else if ((t == CT_PERCENT || u == CT_PERCENT) && (m & ~(unsigned)CT_PERCENT) &&
                         ((m & ~(unsigned)CT_PERCENT) & ((m & ~(unsigned)CT_PERCENT) - 1)) == 0)
                    t = m; /* percent mixed with angle, time, ...: resolved by the property */
                else
                    return CT_INVALID;
            }
        }
        return t;
    }
    case AU_CALC_PRODUCT: {
        for (uint32_t i = 0; i < v->n; i++) {
            const AuCssValue *it = &v->u.items[i];
            unsigned u;
            if (it->type == AU_CSSV_CALC && it->unit == AU_CALC_INVERT) {
                u = css_value_type(&it->u.items[0]);
                if (u != CT_NUMBER)
                    return CT_INVALID; /* division by a dimension */
                continue;
            }
            u = css_value_type(it);
            if (u & CT_INVALID)
                return CT_INVALID;
            if (u) {
                if (t)
                    return CT_INVALID;
                t = u;
            }
        }
        return t;
    }
    case AU_CALC_NEGATE:
    case AU_CALC_ABS:
        return css_value_type(&v->u.items[0]);
    case AU_CALC_INVERT:
        return css_value_type(&v->u.items[0]) == CT_NUMBER ? CT_NUMBER : CT_INVALID;
    case AU_CALC_SIGN:
        return (css_value_type(&v->u.items[0]) & CT_INVALID) ? CT_INVALID : CT_NUMBER;
    case AU_CALC_SIN:
    case AU_CALC_COS:
    case AU_CALC_TAN: {
        unsigned u = css_value_type(&v->u.items[0]);
        return (u == CT_NUMBER || u == CT_ANGLE) ? CT_NUMBER : CT_INVALID;
    }
    case AU_CALC_ASIN:
    case AU_CALC_ACOS:
    case AU_CALC_ATAN:
        return css_value_type(&v->u.items[0]) == CT_NUMBER ? CT_ANGLE : CT_INVALID;
    case AU_CALC_ATAN2: {
        unsigned u = css_value_type(&v->u.items[0]), w = css_value_type(&v->u.items[1]);
        return (u & CT_INVALID) || u != w ? CT_INVALID : CT_ANGLE;
    }
    default: /* pow, sqrt, log, exp */
        for (uint32_t i = 0; i < v->n; i++)
            if (css_value_type(&v->u.items[i]) != CT_NUMBER)
                return CT_INVALID;
        return CT_NUMBER;
    }
}

/* ---- Calc simplification ----
 * Folds what can be folded: sums of like units, products by numbers,
 * comparisons of like units, functions of numbers. Leaves in different
 * relative units (em, vw, %) stay separate until computed. */

static bool leaf_canon(const AuCssValue *v, float *val, int *key)
{
    if (v->type == AU_CSSV_NUMBER) {
        *val = v->u.num;
        *key = -1;
        return true;
    }
    if (v->type == AU_CSSV_PERCENT) {
        *val = v->u.num;
        *key = -2;
        return true;
    }
    if (v->type == AU_CSSV_DIMENSION) {
        float f;
        int c;
        if (css_unit_canonical(v->unit, &f, &c)) {
            *val = v->u.num * f;
            *key = c;
        } else {
            *val = v->u.num;
            *key = v->unit;
        }
        return true;
    }
    return false;
}

static void set_leaf(AuCssValue *v, float val, int key)
{
    memset(v, 0, sizeof *v);
    if (key == -1) {
        v->type = AU_CSSV_NUMBER;
    } else if (key == -2) {
        v->type = AU_CSSV_PERCENT;
    } else {
        v->type = AU_CSSV_DIMENSION;
        v->unit = (uint8_t)key;
    }
    v->u.num = val;
}

static bool is_leaf(const AuCssValue *v)
{
    return v->type == AU_CSSV_NUMBER || v->type == AU_CSSV_PERCENT || v->type == AU_CSSV_DIMENSION;
}

static double eval_op(int op, const double *x, uint32_t n)
{
    switch (op) {
    case AU_CALC_ABS: return fabs(x[0]);
    case AU_CALC_SIGN: return x[0] > 0 ? 1 : x[0] < 0 ? -1 : x[0];
    case AU_CALC_SIN: return sin(x[0]);
    case AU_CALC_COS: return cos(x[0]);
    case AU_CALC_TAN: return tan(x[0]);
    case AU_CALC_ASIN: return asin(x[0]) * 180 / 3.14159265358979;
    case AU_CALC_ACOS: return acos(x[0]) * 180 / 3.14159265358979;
    case AU_CALC_ATAN: return atan(x[0]) * 180 / 3.14159265358979;
    case AU_CALC_ATAN2: return atan2(x[0], x[1]) * 180 / 3.14159265358979;
    case AU_CALC_POW: return pow(x[0], x[1]);
    case AU_CALC_SQRT: return sqrt(x[0]);
    case AU_CALC_EXP: return exp(x[0]);
    case AU_CALC_LOG: return n == 2 ? log(x[0]) / log(x[1]) : log(x[0]);
    case AU_CALC_HYPOT: {
        double s = 0;
        for (uint32_t i = 0; i < n; i++)
            s += x[i] * x[i];
        return sqrt(s);
    }
    case AU_CALC_MOD: {
        if (x[1] == 0)
            return NAN;
        double r = fmod(x[0], x[1]);
        if (r != 0 && ((r < 0) != (x[1] < 0)))
            r += x[1];
        return r;
    }
    case AU_CALC_REM: return x[1] == 0 ? NAN : fmod(x[0], x[1]);
    case AU_CALC_ROUND_NEAREST: return x[1] == 0 ? NAN : floor(x[0] / x[1] + 0.5) * x[1];
    case AU_CALC_ROUND_UP: return x[1] == 0 ? NAN : ceil(x[0] / x[1]) * x[1];
    case AU_CALC_ROUND_DOWN: return x[1] == 0 ? NAN : floor(x[0] / x[1]) * x[1];
    case AU_CALC_ROUND_TO_ZERO: return x[1] == 0 ? NAN : trunc(x[0] / x[1]) * x[1];
    default: return NAN;
    }
}

/* Simplifies v in place (children first). Iteration over the tree uses
 * recursion bounded by the parse depth limit. */
static void simplify(CssArena *a, AuCssValue *v)
{
    if (v->type != AU_CSSV_CALC)
        return;
    for (uint32_t i = 0; i < v->n; i++)
        simplify(a, &v->u.items[i]);
    int op = v->unit;
    switch (op) {
    case AU_CALC_NEGATE:
        if (is_leaf(&v->u.items[0])) {
            AuCssValue c = v->u.items[0];
            c.u.num = -c.u.num;
            *v = c;
        } else if (v->u.items[0].type == AU_CSSV_CALC && v->u.items[0].unit == AU_CALC_SUM) {
            AuCssValue s = v->u.items[0];
            for (uint32_t i = 0; i < s.n; i++) {
                AuCssValue *it = &s.u.items[i];
                if (is_leaf(it)) {
                    it->u.num = -it->u.num;
                } else {
                    AuCssValue *ng = calc_node(a, AU_CALC_NEGATE, 1);
                    ng->u.items[0] = *it;
                    *it = *ng;
                }
            }
            *v = s;
        }
        return;
    case AU_CALC_INVERT:
        if (v->u.items[0].type == AU_CSSV_NUMBER) {
            float x = v->u.items[0].u.num;
            set_leaf(v, 1 / x, -1);
        }
        return;
    case AU_CALC_SUM: {
        /* flatten nested sums, then combine like leaves */
        uint32_t total = 0;
        for (uint32_t i = 0; i < v->n; i++)
            total += (v->u.items[i].type == AU_CSSV_CALC && v->u.items[i].unit == AU_CALC_SUM)
                         ? v->u.items[i].n : 1;
        AuCssValue *flat = css_arena_alloc(a, total * sizeof(AuCssValue));
        uint32_t k = 0;
        for (uint32_t i = 0; i < v->n; i++) {
            const AuCssValue *it = &v->u.items[i];
            if (it->type == AU_CSSV_CALC && it->unit == AU_CALC_SUM)
                for (uint32_t j = 0; j < it->n; j++)
                    flat[k++] = it->u.items[j];
            else
                flat[k++] = *it;
        }
        uint32_t out = 0;
        for (uint32_t i = 0; i < k; i++) {
            float val;
            int key;
            if (!leaf_canon(&flat[i], &val, &key)) {
                flat[out++] = flat[i];
                continue;
            }
            uint32_t j;
            for (j = 0; j < out; j++) {
                float v2;
                int k2;
                if (leaf_canon(&flat[j], &v2, &k2) && k2 == key) {
                    set_leaf(&flat[j], v2 + val, key);
                    break;
                }
            }
            if (j == out)
                set_leaf(&flat[out++], val, key);
        }
        if (out == 1) {
            *v = flat[0];
        } else {
            v->u.items = flat;
            v->n = out;
        }
        return;
    }
    case AU_CALC_PRODUCT: {
        double num = 1;
        AuCssValue *other = NULL;
        uint32_t nother = 0;
        for (uint32_t i = 0; i < v->n; i++) {
            AuCssValue *it = &v->u.items[i];
            if (it->type == AU_CSSV_NUMBER)
                num *= it->u.num;
            else {
                other = it;
                nother++;
            }
        }
        if (nother == 0) {
            set_leaf(v, (float)num, -1);
        } else if (nother == 1) {
            if (is_leaf(other)) {
                AuCssValue c = *other;
                c.u.num = (float)(c.u.num * num);
                *v = c;
            } else if (other->type == AU_CSSV_CALC && other->unit == AU_CALC_SUM) {
                bool all = true;
                for (uint32_t i = 0; i < other->n; i++)
                    all = all && is_leaf(&other->u.items[i]);
                if (all) {
                    AuCssValue s = *other;
                    for (uint32_t i = 0; i < s.n; i++)
                        s.u.items[i].u.num = (float)(s.u.items[i].u.num * num);
                    *v = s;
                }
            }
        }
        return;
    }
    case AU_CALC_MIN:
    case AU_CALC_MAX: {
        float best = 0;
        int bkey = 0;
        for (uint32_t i = 0; i < v->n; i++) {
            float val;
            int key;
            if (!leaf_canon(&v->u.items[i], &val, &key))
                return;
            if (key >= 0 && !css_unit_canonical(key, &(float){0}, &(int){0}))
                return; /* relative units compare only once computed */
            if (key == -2)
                return;
            if (i == 0) {
                best = val;
                bkey = key;
            } else {
                if (key != bkey)
                    return;
                if (op == AU_CALC_MIN ? val < best : val > best)
                    best = val;
            }
        }
        set_leaf(v, best, bkey);
        return;
    }
    case AU_CALC_CLAMP: {
        float val[3];
        int key[3], k = -100;
        for (int i = 0; i < 3; i++) {
            if (css_is_kw(&v->u.items[i], AU_K_none)) {
                key[i] = -100;
                continue;
            }
            if (!leaf_canon(&v->u.items[i], &val[i], &key[i]) || key[i] == -2)
                return;
            if (key[i] >= 0 && !css_unit_canonical(key[i], &(float){0}, &(int){0}))
                return;
            if (k != -100 && key[i] != k)
                return;
            k = key[i];
        }
        float r = val[1];
        if (key[2] != -100 && r > val[2])
            r = val[2];
        if (key[0] != -100 && r < val[0])
            r = val[0];
        set_leaf(v, r, k);
        return;
    }
    default: {
        /* functions of numbers or of one canonical unit */
        double x[32] = {0};
        int key = -1000;
        if (v->n == 0 || v->n > 32)
            return;
        for (uint32_t i = 0; i < v->n; i++) {
            float val;
            int k;
            if (!leaf_canon(&v->u.items[i], &val, &k) || k == -2)
                return;
            if (k >= 0 && !css_unit_canonical(k, &(float){0}, &(int){0}))
                return;
            if (key != -1000 && k != key && op != AU_CALC_ROUND_NEAREST && op != AU_CALC_ROUND_UP &&
                op != AU_CALC_ROUND_DOWN && op != AU_CALC_ROUND_TO_ZERO)
                return;
            if (key == -1000)
                key = k;
            x[i] = k == AU_UNIT_DEG && (op == AU_CALC_SIN || op == AU_CALC_COS || op == AU_CALC_TAN)
                       ? val * 3.14159265358979 / 180 : val;
        }
        double r = eval_op(op, x, v->n);
        int rk;
        switch (op) {
        case AU_CALC_SIN: case AU_CALC_COS: case AU_CALC_TAN: case AU_CALC_SIGN: rk = -1; break;
        case AU_CALC_ASIN: case AU_CALC_ACOS: case AU_CALC_ATAN: case AU_CALC_ATAN2: rk = AU_UNIT_DEG; break;
        default: rk = key; break;
        }
        set_leaf(v, (float)r, rk);
        return;
    }
    }
}

void css_calc_simplify(CssArena *a, AuCssValue *v);
void css_calc_simplify(CssArena *a, AuCssValue *v) { simplify(a, v); }

/* ---- Generic component parsing ---- */

bool css_tokens_have_var(const CssTokens *tk, uint32_t a, uint32_t b)
{
    for (uint32_t i = a; i < b; i++)
        if (tk->t[i].type == TK_FUNCTION && (tk->t[i].kw == AU_K_var || tk->t[i].kw == AU_K_env))
            return true;
    return false;
}

static AuCssValue *str_value(CssParseCtx *cx, int type, const CssTokens *tk, const CssToken *t)
{
    AuCssValue *v = css_new_value(cx->arena, type);
    v->u.str = css_arena_strndup(cx->arena, css_tok_str(tk, t), t->len);
    v->n = t->len;
    return v;
}

static bool is_generic_fn(int kw)
{
    switch (kw) {
    case AU_K_linear_gradient: case AU_K_radial_gradient: case AU_K_conic_gradient:
    case AU_K_repeating_linear_gradient: case AU_K_repeating_radial_gradient:
    case AU_K_repeating_conic_gradient: case AU_K__webkit_linear_gradient: case AU_K__webkit_radial_gradient:
    case AU_K__webkit_repeating_linear_gradient: case AU_K__webkit_repeating_radial_gradient:
    case AU_K_image_set: case AU_K__webkit_image_set: case AU_K_cross_fade: case AU_K_image:
    case AU_K_matrix: case AU_K_matrix3d: case AU_K_translate: case AU_K_translatex: case AU_K_translatey:
    case AU_K_translatez: case AU_K_translate3d: case AU_K_scale: case AU_K_scalex: case AU_K_scaley:
    case AU_K_scalez: case AU_K_scale3d: case AU_K_rotate: case AU_K_rotatex: case AU_K_rotatey:
    case AU_K_rotatez: case AU_K_rotate3d: case AU_K_skew: case AU_K_skewx: case AU_K_skewy:
    case AU_K_perspective: case AU_K_steps: case AU_K_cubic_bezier: case AU_K_linear: case AU_K_repeat:
    case AU_K_minmax: case AU_K_fit_content: case AU_K_counter: case AU_K_counters: case AU_K_blur:
    case AU_K_brightness: case AU_K_contrast: case AU_K_drop_shadow: case AU_K_grayscale:
    case AU_K_hue_rotate: case AU_K_invert: case AU_K_opacity: case AU_K_saturate: case AU_K_sepia:
    case AU_K_inset: case AU_K_circle: case AU_K_ellipse: case AU_K_polygon: case AU_K_path: case AU_K_rect:
    case AU_K_xywh: case AU_K_local: case AU_K_format: case AU_K_tech: case AU_K_selector: case AU_K_layer:
    case AU_K_supports: case AU_K_src: case AU_K_element: case AU_K_type:
        return true;
    default:
        return false;
    }
}

static AuCssValue *parse_value_range(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b);

AuCssValue *css_parse_component(CssParseCtx *cx, const CssTokens *tk, uint32_t *i, uint32_t b)
{
    const CssToken *t = &tk->t[*i];
    CssArena *ar = cx->arena;
    switch (t->type) {
    case TK_IDENT:
        (*i)++;
        if (t->kw)
            return css_new_kw(ar, t->kw);
        return str_value(cx, AU_CSSV_IDENT, tk, t);
    case TK_STRING:
        (*i)++;
        return str_value(cx, AU_CSSV_STRING, tk, t);
    case TK_URL:
        (*i)++;
        return str_value(cx, AU_CSSV_URL, tk, t);
    case TK_NUMBER:
        (*i)++;
        return css_new_num(ar, AU_CSSV_NUMBER, t->num, (t->flags & TF_INTEGER) ? 1 : 0);
    case TK_PERCENT:
        (*i)++;
        return css_new_num(ar, AU_CSSV_PERCENT, t->num, 0);
    case TK_DIMENSION:
        if (t->unit == AU_UNIT_UNKNOWN)
            return NULL;
        (*i)++;
        return css_new_num(ar, AU_CSSV_DIMENSION, t->num, t->unit);
    case TK_HASH: {
        AuColor c;
        if (!css_color_from_hex(css_tok_str(tk, t), t->len, &c))
            return NULL;
        (*i)++;
        AuCssValue *v = css_new_value(ar, AU_CSSV_COLOR);
        v->u.rgba = c;
        return v;
    }
    case TK_DELIM:
        if (t->num == '/') {
            (*i)++;
            AuCssValue *v = css_new_value(ar, AU_CSSV_DELIM);
            v->unit = '/';
            return v;
        }
        return NULL;
    case TK_LBRACKET: {
        /* [ident ...] for grid line names */
        uint32_t end = t->len;
        if (end > b || tk->t[end - 1].type != TK_RBRACKET)
            return NULL;
        uint32_t n = 0;
        for (uint32_t j = *i + 1; j < end - 1; j++) {
            if (tk->t[j].type == TK_WS)
                continue;
            if (tk->t[j].type != TK_IDENT)
                return NULL;
            n++;
        }
        AuCssValue *v = css_new_list(ar, AU_CSSV_FUNCTION, n);
        v->unit = '[';
        uint32_t k = 0;
        for (uint32_t j = *i + 1; j < end - 1; j++)
            if (tk->t[j].type == TK_IDENT) {
                AuCssValue *s = str_value(cx, AU_CSSV_IDENT, tk, &tk->t[j]);
                v->u.items[k++] = *s;
            }
        *i = end;
        return v;
    }
    case TK_FUNCTION: {
        uint32_t end = t->len;
        if (end > b)
            return NULL;
        bool closed = end > *i + 1 && tk->t[end - 1].type == TK_RPAREN;
        uint32_t ib = closed ? end - 1 : end;
        if (is_math_fn(t->kw)) {
            MathP m = {cx, tk};
            AuCssValue *v = math_fn(&m, *i);
            if (!v || (css_value_type(v) & CT_INVALID))
                return NULL;
            simplify(ar, v);
            if (is_leaf(v))
                v->n = 1; /* remember it came from calc(): range checks clamp instead */
            else if (v->type != AU_CSSV_CALC) {
                AuCssValue *w = calc_node(ar, AU_CALC_SUM, 1);
                w->u.items[0] = *v;
                v = w;
            }
            *i = end;
            return v;
        }
        if (t->kw == AU_K_url || t->kw == AU_K_src) {
            uint32_t j = *i + 1;
            skip_ws(tk, &j, ib);
            if (j >= ib || tk->t[j].type != TK_STRING)
                return NULL;
            const CssToken *s = &tk->t[j];
            j++;
            skip_ws(tk, &j, ib);
            if (j != ib)
                return NULL; /* url modifiers are not supported */
            *i = end;
            return str_value(cx, AU_CSSV_URL, tk, s);
        }
        if (t->kw == AU_K_var || t->kw == AU_K_env)
            return NULL;
        if (t->kw == AU_K_attr && !cx->in_content)
            return NULL;
        if (!t->kw || (!is_color_fn(t->kw) && !is_generic_fn(t->kw) && t->kw != AU_K_attr))
            return NULL;
        if (++parse_depth > CSS_MAX_DEPTH) {
            parse_depth--;
            return NULL;
        }
        /* arguments: one per comma-separated group */
        uint32_t nargs = 0;
        uint32_t j = *i + 1;
        bool any = false;
        for (uint32_t k = j; k < ib;) {
            int ty = tk->t[k].type;
            if (ty == TK_COMMA) {
                nargs++;
                k++;
                continue;
            }
            if (ty != TK_WS)
                any = true;
            k = (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET || ty == TK_LBRACE) ? tk->t[k].len : k + 1;
        }
        if (any || nargs)
            nargs++;
        AuCssValue *fn = css_new_list(ar, AU_CSSV_FUNCTION, nargs);
        fn->kw = t->kw;
        uint32_t start = j, ai = 0;
        bool ok = true;
        for (uint32_t k = j; ok && k <= ib;) {
            if (k == ib || tk->t[k].type == TK_COMMA) {
                AuCssValue *arg = parse_value_range(cx, tk, start, k);
                if (!arg) {
                    ok = false;
                    break;
                }
                fn->u.items[ai++] = *arg;
                if (k == ib)
                    break;
                k++;
                start = k;
                continue;
            }
            int ty = tk->t[k].type;
            k = (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET || ty == TK_LBRACE) ? tk->t[k].len : k + 1;
        }
        parse_depth--;
        if (!ok || ai != nargs)
            return NULL;
        *i = end;
        if (is_color_fn(t->kw)) {
            AuColor c;
            if (!parse_color_fn(fn, &c))
                return NULL;
            AuCssValue *v = css_new_value(ar, AU_CSSV_COLOR);
            v->u.rgba = c;
            return v;
        }
        return fn;
    }
    default:
        return NULL;
    }
}

/* Parses [a, b) as space-separated components without commas; returns a
 * LIST, a single component, or NULL (also for an empty range). */
static AuCssValue *parse_space_list(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuCssValue *items[128];
    uint32_t n = 0;
    uint32_t i = a;
    while (i < b) {
        if (tk->t[i].type == TK_WS) {
            i++;
            continue;
        }
        if (n >= CSS_ARRAY_LEN(items))
            return NULL;
        AuCssValue *v = css_parse_component(cx, tk, &i, b);
        if (!v)
            return NULL;
        items[n++] = v;
    }
    if (n == 0)
        return NULL;
    if (n == 1)
        return items[0];
    AuCssValue *l = css_new_list(cx->arena, AU_CSSV_LIST, n);
    for (uint32_t k = 0; k < n; k++)
        l->u.items[k] = *items[k];
    return l;
}

static AuCssValue *parse_value_range(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t ncomma = 0;
    for (uint32_t i = a; i < b;) {
        int ty = tk->t[i].type;
        if (ty == TK_COMMA)
            ncomma++;
        i = (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET || ty == TK_LBRACE) ? tk->t[i].len : i + 1;
    }
    if (!ncomma)
        return parse_space_list(cx, tk, a, b);
    AuCssValue *l = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, ncomma + 1);
    uint32_t start = a, k = 0;
    for (uint32_t i = a; i <= b;) {
        if (i == b || tk->t[i].type == TK_COMMA) {
            AuCssValue *v = parse_space_list(cx, tk, start, i);
            if (!v)
                return NULL;
            l->u.items[k++] = *v;
            if (i == b)
                break;
            i++;
            start = i;
            continue;
        }
        int ty = tk->t[i].type;
        i = (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET || ty == TK_LBRACE) ? tk->t[i].len : i + 1;
    }
    return l;
}

AuCssValue *css_parse_value(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b)
{
    parse_depth = 0;
    return parse_value_range(cx, tk, a, b);
}

/* ---- Trees: copy, size, pack, compare ---- */

static size_t align8(size_t n) { return (n + 7) & ~(size_t)7; }

static size_t extra_size(const AuCssValue *v)
{
    if (is_string_type(v->type))
        return align8(v->n + 1);
    if (!is_tree_type(v->type) && !(v->type == AU_CSSV_FUNCTION))
        return 0;
    size_t s = align8(v->n * sizeof(AuCssValue));
    for (uint32_t i = 0; i < v->n; i++)
        s += extra_size(&v->u.items[i]);
    return s;
}

size_t css_value_tree_size(const AuCssValue *v) { return sizeof(AuCssValue) + extra_size(v); }

/* Copies v's children and strings into *p, advancing it; dst is the copy
 * of v itself. */
static void pack_into(AuCssValue *dst, const AuCssValue *v, char **p)
{
    *dst = *v;
    if (is_string_type(v->type)) {
        char *s = *p;
        memcpy(s, v->u.str, v->n);
        s[v->n] = 0;
        dst->u.str = s;
        *p += align8(v->n + 1);
        return;
    }
    if (!is_tree_type(v->type))
        return;
    AuCssValue *items = (AuCssValue *)*p;
    *p += align8(v->n * sizeof(AuCssValue));
    dst->u.items = v->n ? items : NULL;
    for (uint32_t i = 0; i < v->n; i++)
        pack_into(&items[i], &v->u.items[i], p);
}

AuCssBlob *css_blob_pack(const AuCssValue *v, const char *base_url)
{
    size_t hdr = align8(sizeof(AuCssBlob));
    size_t ex = extra_size(v);
    size_t blen = base_url ? strlen(base_url) + 1 : 0;
    size_t size = hdr + ex + blen;
    AuCssBlob *b = au_alloc(size);
    b->refs = 1;
    b->size = (uint32_t)size;
    char *p = (char *)b + hdr;
    pack_into(&b->value, v, &p);
    if (base_url) {
        memcpy(p, base_url, blen);
        b->base_url = p;
    } else {
        b->base_url = NULL;
    }
    return b;
}

/* While a sheet is parsed, declared values that are equal share one blob
 * (shorthands fill in the same initial lists again and again). The table
 * holds a reference on each blob until the parse ends; declared blobs are
 * never changed. */
static struct {
    AuCssBlob **slots;
    uint32_t size, count;
    int depth;
} blob_intern;

void css_blob_intern_begin(void) { blob_intern.depth++; }

void css_blob_intern_end(void)
{
    if (--blob_intern.depth > 0)
        return;
    for (uint32_t i = 0; i < blob_intern.size; i++)
        au_css_blob_release(blob_intern.slots[i]);
    au_free(blob_intern.slots);
    blob_intern.slots = NULL;
    blob_intern.size = blob_intern.count = 0;
    blob_intern.depth = 0;
}

static uint32_t value_hash(const AuCssValue *v, uint32_t h)
{
    h = (h ^ v->type) * 16777619u;
    h = (h ^ v->unit) * 16777619u;
    h = (h ^ v->kw) * 16777619u;
    h = (h ^ v->n) * 16777619u;
    if (is_string_type(v->type)) {
        for (uint32_t i = 0; i < v->n; i++)
            h = (h ^ (unsigned char)v->u.str[i]) * 16777619u;
    } else if (is_tree_type(v->type)) {
        for (uint32_t i = 0; i < v->n; i++)
            h = value_hash(&v->u.items[i], h);
    } else if (v->type == AU_CSSV_COLOR) {
        h = (h ^ v->u.rgba) * 16777619u;
    } else if (v->type != AU_CSSV_KEYWORD && v->type != AU_CSSV_DELIM) {
        uint32_t bits;
        float f = v->u.num == 0 ? 0 : v->u.num; /* -0 equals 0 */
        memcpy(&bits, &f, sizeof bits);
        h = (h ^ bits) * 16777619u;
    }
    return h;
}

AuCssBlob *css_blob_pack_shared(const AuCssValue *v)
{
    if (blob_intern.depth <= 0)
        return css_blob_pack(v, NULL);
    if (blob_intern.count * 2 >= blob_intern.size) {
        uint32_t size = blob_intern.size ? blob_intern.size * 2 : 256;
        AuCssBlob **slots = au_calloc(size * sizeof *slots);
        for (uint32_t i = 0; i < blob_intern.size; i++) {
            AuCssBlob *o = blob_intern.slots[i];
            if (!o)
                continue;
            uint32_t k = value_hash(&o->value, 2166136261u) & (size - 1);
            while (slots[k])
                k = (k + 1) & (size - 1);
            slots[k] = o;
        }
        au_free(blob_intern.slots);
        blob_intern.slots = slots;
        blob_intern.size = size;
    }
    uint32_t h = value_hash(v, 2166136261u) & (blob_intern.size - 1);
    for (; blob_intern.slots[h]; h = (h + 1) & (blob_intern.size - 1))
        if (css_value_equal(&blob_intern.slots[h]->value, v))
            return au_css_blob_retain(blob_intern.slots[h]);
    AuCssBlob *b = css_blob_pack(v, NULL);
    blob_intern.slots[h] = au_css_blob_retain(b);
    blob_intern.count++;
    return b;
}

AuCssBlob *au_css_blob_retain(AuCssBlob *b)
{
    if (b)
        b->refs++;
    return b;
}

void au_css_blob_release(AuCssBlob *b)
{
    if (b && --b->refs == 0)
        au_free(b);
}

AuCssValue *css_value_copy(CssArena *a, const AuCssValue *v)
{
    size_t ex = extra_size(v);
    AuCssValue *dst = css_arena_alloc(a, sizeof(AuCssValue) + ex);
    char *p = (char *)(dst + 1);
    pack_into(dst, v, &p);
    return dst;
}

bool css_value_equal(const AuCssValue *a, const AuCssValue *b)
{
    if (a == b)
        return true;
    if (!a || !b || a->type != b->type || a->unit != b->unit || a->kw != b->kw || a->n != b->n)
        return false;
    if (is_string_type(a->type))
        return memcmp(a->u.str, b->u.str, a->n) == 0;
    if (is_tree_type(a->type)) {
        for (uint32_t i = 0; i < a->n; i++)
            if (!css_value_equal(&a->u.items[i], &b->u.items[i]))
                return false;
        return true;
    }
    if (a->type == AU_CSSV_COLOR)
        return a->u.rgba == b->u.rgba;
    if (a->type == AU_CSSV_KEYWORD || a->type == AU_CSSV_DELIM)
        return true;
    return a->u.num == b->u.num || (a->u.num != a->u.num && b->u.num != b->u.num);
}

bool css_value_needs_compute(const AuCssValue *v)
{
    switch (v->type) {
    case AU_CSSV_DIMENSION: {
        float f;
        int c;
        return !css_unit_canonical(v->unit, &f, &c) || c != v->unit;
    }
    case AU_CSSV_CALC:
        return true;
    case AU_CSSV_IDENT:
        return false;
    case AU_CSSV_FUNCTION:
        if (v->kw == AU_K_attr)
            return true;
        /* fall through */
    case AU_CSSV_LIST:
    case AU_CSSV_COMMA_LIST:
        for (uint32_t i = 0; i < v->n; i++)
            if (css_value_needs_compute(&v->u.items[i]))
                return true;
        return false;
    case AU_CSSV_NUMBER:
    case AU_CSSV_PERCENT:
        return v->n != 0; /* folded calc: range clamping at compute */
    default:
        return false;
    }
}

/* ---- Debug dump ---- */

void css_debug_dump_value(const AuCssValue *v, AuBuf *out) { au_css_value_serialize(v, out); }
