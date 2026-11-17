/* Fonts and the rest of platform.h (docs/gtk.md, "Fonts").
 *
 * The system's TrueType faces come from fontconfig and are registered
 * lazily (a file is read the first time a page uses one of its faces);
 * the bundled faces in the assets' fonts folder come after them, so a
 * system face of the same family wins. The generic families map to what
 * fontconfig picks for serif, sans-serif, monospace and system-ui. */
#include "gtk.h"

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>

static AuPosixFontDir *system_dir, *bundled_dir;

/* Faces the engine draws: TrueType outlines (CFF is not drawn), and
 * whole faces rather than a variable font's named instances. */
static void add_system_fonts(AuFontSet *set)
{
    if (!FcInit())
        return;
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_FAMILY, FC_WEIGHT, FC_SLANT, FC_FONTFORMAT, NULL);
    FcFontSet *fs = FcFontList(NULL, pat, os);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    if (!fs)
        return;
    system_dir = au_posix_font_dir_new(set, 0);
    for (int i = 0; i < fs->nfont; i++) {
        FcPattern *p = fs->fonts[i];
        FcChar8 *file, *format;
        int index = 0, weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
        if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch)
            continue;
        if (FcPatternGetString(p, FC_FONTFORMAT, 0, &format) != FcResultMatch || strcmp((char *)format, "TrueType"))
            continue;
        FcPatternGetInteger(p, FC_INDEX, 0, &index);
        if (index >> 16)
            continue; /* a named instance of a variable font: the face itself is listed too */
        FcPatternGetInteger(p, FC_WEIGHT, 0, &weight);
        FcPatternGetInteger(p, FC_SLANT, 0, &slant);
        int css_weight = FcWeightToOpenType(weight);
        if (css_weight < 1 || css_weight > 1000)
            css_weight = 400;
        FcChar8 *family;
        for (int f = 0; FcPatternGetString(p, FC_FAMILY, f, &family) == FcResultMatch; f++) {
            if (strlen((char *)family) >= 64)
                continue;
            au_posix_font_dir_add(system_dir, (const char *)file, (const char *)family, css_weight,
                                  slant != FC_SLANT_ROMAN, index);
        }
    }
    FcFontSetDestroy(fs);
}

/* What fontconfig chooses for name, when the set has that family. */
static void map_generic(AuFontSet *set, AuGenericFamily g, const char *name)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)name);
    if (!pat)
        return;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcFontSet *fs = FcFontSort(NULL, pat, FcFalse, NULL, &res);
    FcPatternDestroy(pat);
    if (!fs)
        return;
    /* The first candidate the engine can draw. */
    for (int i = 0; i < fs->nfont && i < 32; i++) {
        FcChar8 *family, *format;
        if (FcPatternGetString(fs->fonts[i], FC_FONTFORMAT, 0, &format) == FcResultMatch &&
            strcmp((char *)format, "TrueType"))
            continue;
        if (FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &family) != FcResultMatch)
            continue;
        if (au_fontset_has_family(set, (const char *)family, strlen((const char *)family))) {
            au_fontset_set_generic(set, g, (const char *)family);
            break;
        }
    }
    FcFontSetDestroy(fs);
}

void au_platform_load_fonts(AuFontSet *set)
{
    if (!g_getenv("AURA_NO_SYSTEM_FONTS"))
        add_system_fonts(set);
    char dir[1100];
    if (au_gtk_asset_path("fonts", dir, sizeof dir))
        bundled_dir = au_posix_add_font_dir(set, dir, AU_FONT_BUNDLED);
    if (system_dir) {
        map_generic(set, AU_GENERIC_SERIF, "serif");
        map_generic(set, AU_GENERIC_SANS_SERIF, "sans-serif");
        map_generic(set, AU_GENERIC_MONOSPACE, "monospace");
        map_generic(set, AU_GENERIC_SYSTEM_UI, "system-ui");
    }
}

void au_gtk_fonts_free(void)
{
    au_posix_font_dir_free(system_dir);
    au_posix_font_dir_free(bundled_dir);
    system_dir = bundled_dir = NULL;
}

/* The engine does not call these; the port paints through AuPresent. */
AuSurface *au_platform_window_surface(void)
{
    AuGtkWin *w = au_gtk_front();
    return w ? w->surf : NULL;
}

void au_platform_present(int x, int y, int w, int h)
{
    AuGtkWin *win = au_gtk_front();
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    if (win && win->area)
        gtk_widget_queue_draw(win->area);
}
