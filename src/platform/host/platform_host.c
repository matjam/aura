/* The host platform: headless, for development and tests on macOS (and
 * any POSIX system). There is no window; the window surface is an
 * offscreen surface the tool writes to a PNG. */
#include "host.h"
#include "fontdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t au_platform_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

bool au_platform_read_file(const char *path, char **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    /* A file whose size is known is read into a block of that size: a
     * buffer grown by doubling keeps up to twice the bytes, and a font's
     * data is kept for good. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        rewind(f);
        if (size > 0) {
            char *d = au_alloc((size_t)size + 1);
            size_t got = fread(d, 1, (size_t)size, f);
            int extra = fgetc(f);
            if (got == (size_t)size && extra == EOF && !ferror(f)) {
                fclose(f);
                d[got] = 0;
                *data = d;
                *len = got;
                return true;
            }
            /* It changed while being read: read it as a stream. */
            au_free(d);
            rewind(f);
        }
    }
    AuBuf b;
    au_buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        au_buf_append(&b, chunk, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        au_buf_free(&b);
        return false;
    }
    *data = au_buf_take(&b, len);
    return true;
}

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
static AuHostFontDir *bundled[16];
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

int au_host_font_option(int argc, char **argv, int i)
{
    const char *a = argv[i];
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

static bool add_file(AuFontSet *set, const char *path)
{
    char *data;
    size_t len;
    if (!au_platform_read_file(path, &data, &len))
        return false;
    /* The data stays allocated for the life of the program. */
    if (au_fontset_add_data(set, data, len) == 0) {
        fprintf(stderr, "aura-host: %s: not a usable TrueType font\n", path);
        au_free(data);
        return false;
    }
    return true;
}

void au_platform_load_fonts(AuFontSet *set)
{
    for (int i = 0; i < nextra; i++)
        if (!add_file(set, extra_fonts[i]))
            fprintf(stderr, "aura-host: cannot load font %s\n", extra_fonts[i]);
    if (use_system_fonts) {
        for (size_t i = 0; i < sizeof system_fonts / sizeof system_fonts[0]; i++)
            add_file(set, system_fonts[i]);
    }
    /* The bundled faces come after the system's, so a system face of the
     * same family wins. Their data lives as long as the process. */
    if (use_bundled_fonts && nbundled < 16)
        bundled[nbundled++] = au_host_add_font_dir(set, au_host_fonts_dir(), AU_FONT_BUNDLED);
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
