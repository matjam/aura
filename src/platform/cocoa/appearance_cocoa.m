/* The Appearance preference on macOS (docs/cocoa.md, "Appearance").
 *
 * The system's light or dark setting decides the scheme while the
 * preference is System: NSApp.effectiveAppearance, followed by key-value
 * observing, so a change in System Settings reaches open pages at once.
 * The chrome follows the preference: Light and Dark set NSApp.appearance
 * to Aqua or Dark Aqua, and System clears it so the system decides. The
 * scheme pages see is the system's, not the forced chrome's, which is why
 * the system's setting is read from the default appearance name rather
 * than from Aura's own windows. */
#import "cocoa.h"

@interface AuAppearanceWatch : NSObject
@end

static AuAppearanceWatch *watch;

/* Whether the system (not Aura's own choice) is set to dark. */
static bool system_is_dark(void)
{
    NSString *style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    return [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame;
}

static void read_system(void)
{
    if (!au_cocoa.browser)
        return;
    bool dark = system_is_dark();
    if (au_cocoa.browser->system_dark != dark) {
        au_browser_set_system_dark(au_cocoa.browser, dark);
        au_cocoa_kick();
    }
}

@implementation AuAppearanceWatch

- (void)observeValueForKeyPath:(NSString *)path ofObject:(id)obj change:(NSDictionary *)change context:(void *)ctx
{
    (void)path;
    (void)obj;
    (void)change;
    (void)ctx;
    read_system();
}

/* The system posts this when the setting changes, also with Automatic. */
- (void)themeChanged:(NSNotification *)n
{
    (void)n;
    /* The defaults are written just before the note is posted. */
    dispatch_async(dispatch_get_main_queue(), ^{
        read_system();
    });
}

@end

void au_cocoa_appearance_apply(void)
{
    if (!au_cocoa.browser)
        return;
    switch (au_cocoa.browser->prefs.color_scheme) {
    case AU_SCHEME_LIGHT: NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua]; break;
    case AU_SCHEME_DARK: NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; break;
    default: NSApp.appearance = nil; break;
    }
}

void au_cocoa_appearance_start(void)
{
    if (watch)
        return;
    watch = [AuAppearanceWatch new];
    [NSApp addObserver:watch forKeyPath:@"effectiveAppearance" options:0 context:NULL];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:watch
                                                        selector:@selector(themeChanged:)
                                                            name:@"AppleInterfaceThemeChangedNotification"
                                                          object:nil];
    read_system();
    au_cocoa_appearance_apply();
}

void au_cocoa_appearance_stop(void)
{
    if (!watch)
        return;
    [NSApp removeObserver:watch forKeyPath:@"effectiveAppearance"];
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:watch];
    watch = nil;
}
