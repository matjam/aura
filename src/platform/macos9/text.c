/* UTF-8 and Mac Roman, Pascal strings, exact menu item text. The
 * conversion (with transliteration of what Mac Roman lacks) is the
 * engine's, in src/browser/macroman.c. */
#include "mac.h"

#include "browser/macroman.h"

#include <string.h>

char *mac_utf8_to_roman(const char *s, size_t *n)
{
    AuBuf b;
    au_buf_init(&b);
    au_macroman_from_utf8(s, strlen(s), &b, '?', 0);
    au_buf_cstr(&b);
    size_t len;
    char *out = au_buf_take(&b, &len);
    if (n)
        *n = len;
    return out;
}

char *mac_roman_to_utf8(const char *s, size_t n)
{
    AuBuf b;
    au_buf_init(&b);
    au_macroman_to_utf8(s, n, &b, 0);
    au_buf_cstr(&b);
    return au_buf_take(&b, NULL);
}

void mac_pstr(const char *utf8, Str255 out)
{
    size_t n;
    char *r = mac_utf8_to_roman(utf8 ? utf8 : "", &n);
    if (n > 255)
        n = 255;
    /* Control characters (a tab or line break in a title) show as boxes
     * in menus and title bars: make them spaces. */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)r[i] < 0x20)
            r[i] = ' ';
    out[0] = (unsigned char)n;
    memcpy(out + 1, r, n);
    au_free(r);
}

void mac_set_item_text(MenuHandle m, short item, const char *utf8)
{
    Str255 p;
    mac_pstr(utf8, p);
    if (p[0] == 0) {
        p[0] = 1;
        p[1] = ' ';
    }
    /* A leading '-' would make the item a separator line. */
    if (p[1] == '-')
        p[1] = 0xD0; /* en dash */
    SetMenuItemText(m, item, p);
}

void mac_append_item(MenuHandle m, const char *utf8)
{
    /* AppendMenu parses metacharacters (; ^ ! < / ( ), so add a
     * placeholder and set the real text afterwards. */
    AppendMenu(m, (ConstStringPtr) "\px");
    mac_set_item_text(m, (short)CountMItems(m), utf8);
}
