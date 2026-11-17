/* The About box: the standard about panel, filled from the shared credits
 * (src/browser/credits.h) and the build's identity (sysinfo.h). Links
 * open in Aura, in a new tab, rather than in another browser: the panel's
 * credits view gets a delegate that hands a clicked link to open_url.
 * When the panel's view cannot be found, the web links open in the
 * system's browser and about:licences is plain text. */
#import "windows_cocoa.h"

#include "browser/credits.h"

#include <string.h>

/* The credits view's delegate: a clicked link opens in Aura. */
@interface AuAboutLinks : NSObject <NSTextViewDelegate>
@end

@implementation AuAboutLinks
- (BOOL)textView:(NSTextView *)view clickedOnLink:(id)link atIndex:(NSUInteger)index
{
    (void)view;
    (void)index;
    NSString *url = [link isKindOfClass:[NSURL class]] ? [(NSURL *)link absoluteString]
                    : [link isKindOfClass:[NSString class]] ? (NSString *)link
                                                            : nil;
    if (!url || !au_cocoa_wh.open_url)
        return NO; /* the system's handler */
    au_cocoa_wh.open_url(au_cocoa_wh.ctx, url.UTF8String, true);
    return YES;
}
@end

static AuAboutLinks *g_links;

static NSString *str(const char *utf8)
{
    NSString *s = utf8 ? [NSString stringWithUTF8String:utf8] : nil;
    return s ? s : @"";
}

static NSDictionary *text_attrs(bool bold)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.alignment = NSTextAlignmentCenter;
    ps.paragraphSpacing = 2;
    CGFloat size = NSFont.smallSystemFontSize;
    return @{
        NSFontAttributeName : bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size],
        NSForegroundColorAttributeName : NSColor.labelColor,
        NSParagraphStyleAttributeName : ps,
    };
}

static void add(NSMutableAttributedString *out, NSString *text, bool bold)
{
    [out appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:text_attrs(bold)]];
}

static void add_link(NSMutableAttributedString *out, NSString *text, NSString *url)
{
    NSMutableDictionary *a = [text_attrs(false) mutableCopy];
    a[NSLinkAttributeName] = [NSURL URLWithString:url];
    [out appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:a]];
}

/* One credit section per kind: a heading, then the names in the table's
 * order. */
static void add_section(NSMutableAttributedString *out, NSString *title, AuCreditKind kind)
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (size_t i = 0; i < au_ncredits; i++)
        if (au_credits[i].kind == kind)
            [names addObject:str(au_credits[i].name)];
    if (!names.count)
        return;
    add(out, @"\n", false);
    add(out, [title stringByAppendingString:@"\n"], true);
    add(out, [[names componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"], false);
}

static NSAttributedString *credits(void)
{
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    add(out, @AU_TAGLINE "\n", false);
    add_link(out, @"Aura on GitHub", @AU_HOMEPAGE);
    add(out, @"\n\n", false);
    add(out, @"Written by\n", true);
    add(out, @AU_AUTHOR "\n", false);
    add_section(out, @"Libraries", AU_CREDIT_LIBRARY);
    add_section(out, @"Fonts", AU_CREDIT_FONT);
    add_section(out, @"Data", AU_CREDIT_DATA);
    add(out, @"\n", false);
    add(out,
        @"Aura is released under the MIT licence. You may use, copy, change and share it freely, as long "
        @"as the copyright notice and the licence go with it. It comes with no warranty.\n\n",
        false);
    add(out, @"The licences of the libraries, fonts and data Aura includes are on the page ", false);
    add_link(out, @"about:licences", @"about:licences");
    add(out, @".", false);
    return out;
}

/* The text view in view's tree that shows text, or nil. */
static NSTextView *find_text_view(NSView *view, NSString *text)
{
    if ([view isKindOfClass:[NSTextView class]] && [((NSTextView *)view).string isEqualToString:text])
        return (NSTextView *)view;
    for (NSView *sub in view.subviews) {
        NSTextView *t = find_text_view(sub, text);
        if (t)
            return t;
    }
    return nil;
}

/* Routes the panel's links to Aura. Returns false when its credits view
 * was not found or already has a delegate. */
static bool route_links(NSString *text)
{
    for (NSWindow *w in NSApp.windows) {
        NSTextView *t = w.contentView ? find_text_view(w.contentView, text) : nil;
        if (!t)
            continue;
        if (t.delegate && t.delegate != (id)g_links)
            return false;
        if (!g_links)
            g_links = [AuAboutLinks new];
        t.delegate = g_links;
        return true;
    }
    return false;
}

void au_cocoa_about_show(void)
{
    /* "0.4.1 (1a2b3c4)": the version, and the commit in parentheses as
     * the panel shows its build number. */
    NSString *identity = str(au_build_identity());
    NSString *build = @AU_VERSION; /* the same as the version: not shown */
    NSRange open = [identity rangeOfString:@" ("];
    if (open.location != NSNotFound && [identity hasSuffix:@")"]) {
        NSUInteger from = NSMaxRange(open);
        build = [identity substringWithRange:NSMakeRange(from, identity.length - 1 - from)];
    }
    NSAttributedString *text = credits();
    NSMutableDictionary *opts = [NSMutableDictionary dictionary];
    opts[NSAboutPanelOptionApplicationName] = @"Aura";
    opts[NSAboutPanelOptionApplicationVersion] = @AU_VERSION;
    opts[NSAboutPanelOptionVersion] = build;
    opts[NSAboutPanelOptionCredits] = text;
    opts[@"Copyright"] = @"Written by " AU_AUTHOR ", with help from Claude.";
    NSImage *icon = NSApp.applicationIconImage;
    if (icon)
        opts[NSAboutPanelOptionApplicationIcon] = icon;
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
    if (!route_links(text.string)) {
        /* No way to open about:licences in Aura: show the credits with it
         * as plain text. */
        NSMutableAttributedString *plain = [text mutableCopy];
        NSRange r = [plain.string rangeOfString:@"about:licences"];
        if (r.location != NSNotFound)
            [plain removeAttribute:NSLinkAttributeName range:r];
        opts[NSAboutPanelOptionCredits] = plain;
        [NSApp orderFrontStandardAboutPanelWithOptions:opts];
    }
    if (au_cocoa_wh.windows_changed)
        au_cocoa_wh.windows_changed(au_cocoa_wh.ctx);
}

bool au_cocoa_about_built(void) { return true; }
