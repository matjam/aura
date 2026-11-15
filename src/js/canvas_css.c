/* CSS values for the canvas bindings: colours and the font shorthand,
 * parsed by src/css. Kept apart from natives_canvas.c so that file needs
 * no CSS types. Colours come back as 0xAARRGGBB (src/paint's order). */
#include "js_int.h"

#include "../css/css.h"

#include <string.h>

bool js_canvas_parse_color(const char *s, size_t len, const void *style, uint32_t *argb);
bool js_canvas_parse_font(const char *s, size_t len, const void *style, char **families, char **text, int *weight,
                          bool *italic, float *size);

static uint32_t to_argb(AuColor c)
{
    return (c >> 8) | (c << 24);
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* style: the canvas element's computed style (an AuStyle) or NULL, for
 * currentcolor. */
bool js_canvas_parse_color(const char *s, size_t len, const void *style, uint32_t *argb)
{
    /* #rgb, #rrggbb and #rrggbbaa without the CSS parser: scripts set a
     * colour before most drawing calls. */
    if (len && s[0] == '#' && (len == 4 || len == 7 || len == 9)) {
        uint32_t v = 0;
        bool ok = true;
        for (size_t i = 1; i < len; i++) {
            int h = hexv((unsigned char)s[i]);
            if (h < 0) {
                ok = false;
                break;
            }
            v = (v << 4) | (uint32_t)h;
        }
        if (ok) {
            if (len == 4)
                *argb = 0xFF000000u | ((v & 0xF00) * 0x1100) | ((v & 0xF0) * 0x110) | ((v & 0xF) * 0x11);
            else if (len == 7)
                *argb = 0xFF000000u | v;
            else
                *argb = (v >> 8) | (v << 24);
            return true;
        }
    }
    AuColor c;
    bool current = false;
    if (!au_css_parse_color_text(s, len, &c, &current))
        return false;
    if (current) {
        const AuStyle *st = style;
        *argb = st ? to_argb(st->inh->color) : 0xFF000000u;
        return true;
    }
    *argb = to_argb(c);
    return true;
}

bool js_canvas_parse_font(const char *s, size_t len, const void *style, char **families, char **text, int *weight,
                          bool *italic, float *size)
{
    AuCssCanvasFont f;
    memset(&f, 0, sizeof f);
    au_buf_init(&f.families);
    au_buf_init(&f.text);
    bool ok = au_css_parse_font_text(s, len, style, &f);
    if (ok) {
        *weight = f.weight;
        *italic = f.italic;
        *size = f.size;
        *families = au_buf_take(&f.families, NULL);
        *text = au_buf_take(&f.text, NULL);
    }
    au_buf_free(&f.families);
    au_buf_free(&f.text);
    return ok;
}
