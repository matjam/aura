/* The host platform: headless, for development and tests on macOS and
 * Linux. There is no window; the window surface is an offscreen surface
 * the tool writes to a PNG. The clock and file reads of platform.h come
 * from src/platform/posix. */
#include "host.h"
#include "platform/posix/posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The macOS system fonts that match the fonts Mac OS 9.2.2 ships, so the
 * host and the guest draw the scene with the same families. */
static const char *const system_fonts[] = {
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Geneva.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Italic.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman Italic.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman Bold Italic.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Italic.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold Italic.ttf",
    "/System/Library/Fonts/Supplemental/Georgia.ttf",
    "/System/Library/Fonts/Supplemental/Georgia Bold.ttf",
    "/System/Library/Fonts/Supplemental/Georgia Italic.ttf",
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
    "/System/Library/Fonts/Supplemental/Verdana Bold.ttf",
    "/System/Library/Fonts/Supplemental/Verdana Italic.ttf",
    "/System/Library/Fonts/Supplemental/Trebuchet MS.ttf",
    "/System/Library/Fonts/Supplemental/Trebuchet MS Bold.ttf",
};

static const char *extra_fonts[64];
static int nextra;
static bool use_system_fonts = true, use_bundled_fonts = true;
static const char *bundled_dir;
static AuPosixFontDir *bundled[16];
static int nbundled;

void au_host_add_font_path(const char *path)
{
    if (nextra < 64)
        extra_fonts[nextra++] = path;
}

void au_host_use_system_fonts(bool on)
{
    use_system_fonts = on;
}

void au_host_use_bundled_fonts(bool on)
{
    use_bundled_fonts = on;
}

void au_host_set_fonts_dir(const char *dir)
{
    bundled_dir = dir;
}

const char *au_host_fonts_dir(void)
{
    if (bundled_dir)
        return bundled_dir;
    const char *env = getenv("AURA_FONTS");
    return env && *env ? env : AURA_FONTS_DIR;
}

static bool host_dark;

bool au_host_dark(void) { return host_dark; }

int au_host_font_option(int argc, char **argv, int i)
{
    const char *a = argv[i];
    if (!strcmp(a, "--color-scheme") && i + 1 < argc) {
        host_dark = !strcmp(argv[i + 1], "dark");
        return 2;
    }
    if (!strcmp(a, "--font") && i + 1 < argc) {
        au_host_add_font_path(argv[i + 1]);
        return 2;
    }
    if (!strcmp(a, "--fonts-dir") && i + 1 < argc) {
        au_host_set_fonts_dir(argv[i + 1]);
        return 2;
    }
    if (!strcmp(a, "--no-system-fonts") || !strcmp(a, "--bundled-only")) {
        au_host_use_system_fonts(false);
        return 1;
    }
    if (!strcmp(a, "--no-bundled-fonts")) {
        au_host_use_bundled_fonts(false);
        return 1;
    }
    return 0;
}

static bool have_family(AuFontSet *set, const char *family)
{
    for (size_t i = 0; i < au_fontset_count(set); i++) {
        const char *f = au_fontset_entry(set, i, NULL, NULL);
        if (au_ieq(f, strlen(f), family, strlen(family)))
            return true;
    }
    return false;
}

static void map_generic(AuFontSet *set, AuGenericFamily g, const char *a, const char *b)
{
    if (have_family(set, a))
        au_fontset_set_generic(set, g, a);
    else if (b && have_family(set, b))
        au_fontset_set_generic(set, g, b);
}

void au_platform_load_fonts(AuFontSet *set)
{
    for (int i = 0; i < nextra; i++)
        if (!au_posix_add_font_file(set, extra_fonts[i]))
            fprintf(stderr, "aura-host: cannot load font %s\n", extra_fonts[i]);
    if (use_system_fonts) {
        for (size_t i = 0; i < sizeof system_fonts / sizeof system_fonts[0]; i++)
            au_posix_add_font_file(set, system_fonts[i]);
    }
    /* The bundled faces come after the system's, so a system face of the
     * same family wins. Their data lives as long as the process. */
    if (use_bundled_fonts && nbundled < 16)
        bundled[nbundled++] = au_posix_add_font_dir(set, au_host_fonts_dir(), AU_FONT_BUNDLED);
    map_generic(set, AU_GENERIC_SERIF, "Times New Roman", "Times");
    map_generic(set, AU_GENERIC_SANS_SERIF, "Helvetica", "Arial");
    map_generic(set, AU_GENERIC_MONOSPACE, "Courier New", "Courier");
    map_generic(set, AU_GENERIC_SYSTEM_UI, "Geneva", "Helvetica");
}

static AuSurface *window;

AuSurface *au_platform_window_surface(void)
{
    return window;
}

void au_host_set_window_surface(AuSurface *s)
{
    window = s;
}

void au_platform_present(int x, int y, int w, int h)
{
    /* Headless: the tool writes the surface to a file instead. */
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
