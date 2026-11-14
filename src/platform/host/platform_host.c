/* The host platform: headless, for development and tests on macOS (and
 * any POSIX system). There is no window; the window surface is an
 * offscreen surface the tool writes to a PNG. */
#include "host.h"

#include <stdio.h>
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
static bool use_system_fonts = true;

void au_host_add_font_path(const char *path)
{
    if (nextra < 64)
        extra_fonts[nextra++] = path;
}

void au_host_use_system_fonts(bool on)
{
    use_system_fonts = on;
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
