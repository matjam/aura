/* Fonts and the rest of platform.h (docs/cocoa.md, "Fonts").
 *
 * The system's TrueType faces come from Core Text and are registered
 * lazily (a file is read the first time a page uses one of its faces);
 * the bundled faces in the assets' fonts folder come after them, so a
 * system face of the same family wins. Aura's own font code reads the
 * files; Core Text only lists them. The generic families map to the
 * first of the usual Mac families the set has. */
#import "cocoa.h"

#import <CoreText/CoreText.h>

#include <stdio.h>
#include <string.h>

static AuPosixFontDir *system_dir, *bundled_dir;

/* Core Text's weight trait (-1 to 1) as a CSS weight. */
static int css_weight(double t)
{
    static const struct {
        double trait;
        int css;
    } map[] = {{-0.80, 100}, {-0.60, 200}, {-0.40, 300}, {0.00, 400}, {0.23, 500},
               {0.30, 600},  {0.40, 700},  {0.56, 800},  {0.62, 900}};
    int best = 400;
    double bd = 10;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        double d = t > map[i].trait ? t - map[i].trait : map[i].trait - t;
        if (d < bd) {
            bd = d;
            best = map[i].css;
        }
    }
    return best;
}

/* Faces the engine draws: TrueType outlines (CFF is not drawn), from
 * files whose faces Core Text lists in file order, so a collection's face
 * index is its place in the list. Families whose names begin with a dot
 * are the system's private faces. Apple Color Emoji's glyphs are bitmaps
 * the engine does not draw; the bundled emoji fonts cover it. */
static void add_system_fonts(AuFontSet *set)
{
    CFArrayRef urls = CTFontManagerCopyAvailableFontURLs();
    if (!urls)
        return;
    system_dir = au_posix_font_dir_new(set, 0);
    for (CFIndex u = 0; u < CFArrayGetCount(urls); u++) {
        NSURL *url = (__bridge NSURL *)CFArrayGetValueAtIndex(urls, u);
        const char *path = url.fileSystemRepresentation;
        if (!path)
            continue;
        CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
        if (!descs)
            continue;
        for (CFIndex i = 0; i < CFArrayGetCount(descs); i++) {
            CTFontDescriptorRef d = CFArrayGetValueAtIndex(descs, i);
            NSNumber *format = CFBridgingRelease(CTFontDescriptorCopyAttribute(d, kCTFontFormatAttribute));
            int f = format.intValue;
            if (f != kCTFontFormatTrueType && f != kCTFontFormatOpenTypeTrueType)
                continue;
            NSString *family = CFBridgingRelease(CTFontDescriptorCopyAttribute(d, kCTFontFamilyNameAttribute));
            if (!family.length || [family hasPrefix:@"."] || [family isEqualToString:@"Apple Color Emoji"])
                continue;
            const char *fam = family.UTF8String;
            if (strlen(fam) >= 64)
                continue;
            NSDictionary *traits = CFBridgingRelease(CTFontDescriptorCopyAttribute(d, kCTFontTraitsAttribute));
            double wt = [traits[(__bridge NSString *)kCTFontWeightTrait] doubleValue];
            uint32_t sym = [traits[(__bridge NSString *)kCTFontSymbolicTrait] unsignedIntValue];
            au_posix_font_dir_add(system_dir, path, fam, css_weight(wt), (sym & kCTFontItalicTrait) != 0, (int)i);
        }
        CFRelease(descs);
    }
    CFRelease(urls);
}

/* The first of names that the set has. */
static void map_generic(AuFontSet *set, AuGenericFamily g, const char *const *names)
{
    for (int i = 0; names[i]; i++) {
        if (au_fontset_has_family(set, names[i], strlen(names[i]))) {
            au_fontset_set_generic(set, g, names[i]);
            return;
        }
    }
}

void au_platform_load_fonts(AuFontSet *set)
{
    if (!getenv("AURA_NO_SYSTEM_FONTS"))
        add_system_fonts(set);
    char dir[1100];
    if (au_cocoa_asset_path("fonts", dir, sizeof dir))
        bundled_dir = au_posix_add_font_dir(set, dir, AU_FONT_BUNDLED);
    if (system_dir) {
        static const char *const serif[] = {"Times New Roman", "Times", "Georgia", NULL};
        static const char *const sans[] = {"Helvetica", "Arial", "Helvetica Neue", NULL};
        static const char *const mono[] = {"Menlo", "Courier New", "Monaco", NULL};
        static const char *const ui[] = {"Helvetica Neue", "Helvetica", "Arial", NULL};
        map_generic(set, AU_GENERIC_SERIF, serif);
        map_generic(set, AU_GENERIC_SANS_SERIF, sans);
        map_generic(set, AU_GENERIC_MONOSPACE, mono);
        map_generic(set, AU_GENERIC_SYSTEM_UI, ui);
    }
}

void au_cocoa_fonts_free(void)
{
    au_posix_font_dir_free(system_dir);
    au_posix_font_dir_free(bundled_dir);
    system_dir = bundled_dir = NULL;
}

/* The engine does not call these; the port paints through AuPresent. */
AuSurface *au_platform_window_surface(void)
{
    AuCocoaWin *w = au_cocoa_front();
    return w ? w->surf : NULL;
}

void au_platform_present(int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    au_cocoa_kick();
}
