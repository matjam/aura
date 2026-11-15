/* The chrome draws tab titles and status text through the font fallback
 * API: with only the bundled fonts, a title mixing Latin, Cyrillic,
 * symbols and emoji shapes without missing glyphs through the chrome's
 * font list, ellipsizes within its width, and paints. */
#include "browser/browser.h"
#include "fontdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

int main(void)
{
    AuFontSet *fonts = au_fontset_new();
    AuHostFontDir *dir = au_host_add_font_dir(fonts, AURA_FONTS_DIR, AU_FONT_BUNDLED);
    CHECK(dir && au_host_font_dir_count(dir) > 0);
    static const char title[] = "Caf\xC3\xA9 \xD0\x96\xD0\xB8\xD0\xB7\xD0\xBD\xD1\x8C \xE2\x8C\x98 \xF0\x9F\x98\x80 "
                                "\xE2\x80\x9Cquoted\xE2\x80\x9D \xCE\xA9";
    AuFontList *l = au_fontlist_parse(fonts, AU_CHROME_LABEL_FAMILIES, 400, false);
    CHECK(l && au_fontlist_primary(l));
    AuGlyphRun r;
    memset(&r, 0, sizeof r);
    au_shape_text_list(l, 12, title, strlen(title), &r);
    CHECK(r.missing == 0);
    au_glyph_run_free(&r);

    char out[256];
    au_chrome_ellipsize_list(l, 12, title, 80, out, sizeof out);
    CHECK(au_text_width_list(l, 12, out, strlen(out)) <= 80.5f);
    CHECK(strlen(out) < strlen(title) && strstr(out, "\xE2\x80\xA6"));
    au_chrome_ellipsize_list(l, 12, title, 2000, out, sizeof out);
    CHECK(!strcmp(out, title));

    /* Paint a window's chrome with that title. */
    AuChromeLayout lay;
    au_chrome_layout(&lay, 640, 400, AU_TABS_TOP, 170, 2, 0);
    AuChromeTab tabs[2] = {{title, false}, {"\xE6\x97\xA5\xE6\x9C\xAC", true}};
    AuChromeState s;
    memset(&s, 0, sizeof s);
    s.tabs = tabs;
    s.active = 0;
    s.dragging = -1;
    s.field_text = title;
    s.draw_field_text = true;
    s.status_text = title;
    s.active_window = true;
    s.label_list = l;
    s.small_list = au_fontlist_parse(fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    AuSurface *surf = au_surface_new(640, 400);
    AuCanvas *c = au_canvas_new(surf);
    au_chrome_paint(c, fonts, &lay, &s, (AuIRect){0, 0, 640, 400});
    au_canvas_free(c);
    au_surface_free(surf);
    au_fontlist_free(s.small_list);
    au_fontlist_free(l);
    au_fontset_free(fonts);
    au_host_font_dir_free(dir);
    printf("chrome_fonts: all tests passed\n");
    return 0;
}
