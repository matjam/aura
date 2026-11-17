/* A browser window: an NSWindow with a unified toolbar (Back, Forward,
 * Reload or Stop, the security icon and address field, New Tab), and a
 * body holding the tab strip, the find bar, the page with its scroll bars
 * and the status line (docs/cocoa.md, "Windows and tabs").
 *
 * The model's window is made with au_browser_window_new_native: its
 * surface holds only the page. The controls show what the au_bw_*
 * accessors report; AuBrowserHost.changed only records which ones to set
 * (AuCocoaWin.pending), and au_cocoa_win_flush sets them and paints, so no
 * control is changed from inside a model call or a control's action. */
#import "cocoa.h"

#import <QuartzCore/QuartzCore.h>

#include <string.h>

#define STRIP_HEIGHT 30
#define TAB_ROW_HEIGHT 28
#define TAB_MAX_WIDTH 230
#define TAB_MIN_WIDTH 70
#define FIND_HEIGHT 32
#define STATUS_HEIGHT 22

static NSMutableArray<AuCocoaWin *> *wins;

NSArray<AuCocoaWin *> *au_cocoa_windows(void)
{
    return wins ? wins : @[];
}

AuCocoaWin *au_cocoa_win_of(AuBrowserWindow *bw)
{
    AuCocoaWin *w = bw ? (__bridge AuCocoaWin *)bw->platform : nil;
    return w && !w->freed ? w : nil;
}

AuCocoaWin *au_cocoa_win_for(NSWindow *nw)
{
    for (AuCocoaWin *w in wins)
        if (w->window == nw && !w->freed)
            return w;
    return nil;
}

AuCocoaWin *au_cocoa_front(void)
{
    for (AuCocoaWin *w in wins)
        if (w == au_cocoa.front && !w->closing)
            return w;
    for (AuCocoaWin *w in wins)
        if (!w->closing)
            return w;
    return nil;
}

static NSImage *symbol(NSString *name, NSString *desc)
{
    NSImage *img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:desc];
    return img ? img : [NSImage imageNamed:NSImageNameStopProgressTemplate];
}

/* A site icon (premultiplied 0xAARRGGBB) as an NSImage. */
static NSImage *icon_image(const AuSurface *s)
{
    if (!s || s->width <= 0 || s->height <= 0)
        return nil;
    size_t w = (size_t)s->width, h = (size_t)s->height;
    NSMutableData *d = [NSMutableData dataWithLength:w * h * 4];
    uint32_t *out = d.mutableBytes;
    for (size_t y = 0; y < h; y++)
        memcpy(out + y * w, s->pixels + y * (size_t)s->stride, w * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)d);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                   kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, dp, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img)
        return nil;
    NSImage *ni = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(16, 16)];
    CGImageRelease(img);
    return ni;
}

/* ---- Small views ---- */

/* A rectangle of one system colour, optionally rounded and outlined;
 * the colours are resolved again when the appearance changes. */
@interface AuFillView : NSView
@property(strong) NSColor *fill, *stroke;
@property CGFloat radius;
@end

@implementation AuFillView

- (instancetype)initWithFill:(NSColor *)fill
{
    self = [super initWithFrame:NSZeroRect];
    self.fill = fill;
    self.wantsLayer = YES;
    return self;
}

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (void)updateLayer
{
    self.layer.backgroundColor = self.fill.CGColor;
    self.layer.cornerRadius = self.radius;
    self.layer.borderColor = self.stroke.CGColor;
    self.layer.borderWidth = self.stroke ? 1 : 0;
}

@end

/* ---- The address field ----
 * A rounded text field with the security icon at its left, Reload or Stop
 * at its right, and a line along its bottom that grows as the page loads,
 * as in Safari. The cell keeps the text clear of the two. */

#define FIELD_LEAD 26
#define FIELD_TRAIL 26

@interface AuAddressCell : NSTextFieldCell
@end

@implementation AuAddressCell

- (NSRect)drawingRectForBounds:(NSRect)r
{
    r.origin.x += FIELD_LEAD;
    r.size.width -= FIELD_LEAD + FIELD_TRAIL;
    if (r.size.width < 0)
        r.size.width = 0;
    return [super drawingRectForBounds:r];
}

@end

@interface AuAddressField : NSTextField {
  @public
    NSImageView *icon;
    NSButton *reload;
    AuFillView *bar;
    float progress; /* 0..1; 0 hides the line */
}
@end

@implementation AuAddressField

+ (Class)cellClass
{
    return [AuAddressCell class];
}

- (void)layout
{
    [super layout];
    NSRect b = self.bounds;
    CGFloat mid = floor(NSMidY(b));
    icon.frame = NSMakeRect(8, mid - 8, 16, 16);
    reload.frame = NSMakeRect(NSMaxX(b) - 24, mid - 10, 20, 20);
    CGFloat inner = b.size.width - 16;
    bar.hidden = progress <= 0;
    /* NSTextField is not flipped: y 0 is the bottom edge */
    bar.frame = NSMakeRect(8, self.isFlipped ? NSMaxY(b) - 3 : 1, inner * (progress > 1 ? 1 : progress), 2);
}

@end

/* ---- The tab strip ----
 * One AuTabItem per tab of the model: its icon or a spinner while it
 * loads, its title, and a close button that shows on hover and on the
 * active tab. A click selects the tab, a drag moves it, a middle click
 * closes it. The strip lays the items out in a row, or in a column for
 * tabs on the left (the tab position preference). */

@class AuTabStrip;

@interface AuTabItem : NSView {
  @public
    __unsafe_unretained AuTabStrip *strip;
    int index;
    bool active, hover;
    bool divider; /* a line at its trailing edge: it and the next are both inactive */
    NSImageView *icon;
    NSProgressIndicator *spinner;
    NSTextField *label;
    NSButton *close;
    NSTrackingArea *tracking;
}
@end

@interface AuTabStrip : NSView {
  @public
    __unsafe_unretained AuCocoaWin *win;
    NSVisualEffectView *material; /* the toolbar's material, so the two read as one bar */
    AuFillView *edge;             /* the separator under the row, or beside the column */
    NSMutableArray<AuTabItem *> *items;
    bool vertical;
    int drag_from;
}
- (void)syncFrom:(AuCocoaWin *)w;
- (int)indexAt:(NSPoint)p;
@end

@implementation AuTabItem

- (instancetype)initWithStrip:(AuTabStrip *)s
{
    self = [super initWithFrame:NSMakeRect(0, 0, 100, TAB_ROW_HEIGHT)];
    strip = s;
    icon = [NSImageView imageViewWithImage:symbol(@"globe", @"Page")];
    icon.imageScaling = NSImageScaleProportionallyDown;
    icon.contentTintColor = NSColor.secondaryLabelColor;
    spinner = [NSProgressIndicator new];
    spinner.style = NSProgressIndicatorStyleSpinning;
    spinner.controlSize = NSControlSizeSmall;
    spinner.displayedWhenStopped = NO;
    label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.cell.usesSingleLineMode = YES;
    close = [NSButton buttonWithImage:symbol(@"xmark", @"Close Tab") target:self action:@selector(closeTab:)];
    close.bordered = NO;
    close.imageScaling = NSImageScaleProportionallyDown;
    close.toolTip = @"Close Tab";
    close.refusesFirstResponder = YES;
    for (NSView *v in @[ icon, spinner, label, close ])
        [self addSubview:v];
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (void)layout
{
    [super layout];
    NSRect b = self.bounds;
    CGFloat midy = NSMidY(b);
    icon.frame = NSMakeRect(8, midy - 8, 16, 16);
    spinner.frame = icon.frame;
    close.frame = NSMakeRect(NSMaxX(b) - 22, midy - 8, 16, 16);
    CGFloat lx = 30, rx = NSMaxX(b) - 26;
    label.frame = NSMakeRect(lx, midy - 8, rx > lx ? rx - lx : 0, 17);
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (tracking)
        [self removeTrackingArea:tracking];
    tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                            options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                                                    NSTrackingInVisibleRect
                                              owner:self
                                           userInfo:nil];
    [self addTrackingArea:tracking];
}

- (void)mouseEntered:(NSEvent *)e
{
    (void)e;
    hover = true;
    close.hidden = NO;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    hover = false;
    close.hidden = !active;
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect r = NSInsetRect(self.bounds, 2, 2);
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6];
    if (active || hover) {
        [[NSColor.labelColor colorWithAlphaComponent:active ? 0.11 : 0.05] setFill];
        [p fill];
    } else if (divider && !strip->vertical) {
        [[NSColor.separatorColor colorWithAlphaComponent:0.8] setFill];
        NSRectFill(NSMakeRect(NSMaxX(self.bounds) - 1, NSMidY(self.bounds) - 7, 1, 14));
    }
}

- (void)closeTab:(id)sender
{
    (void)sender;
    AuCocoaWin *w = strip->win;
    if (!w || w->freed || au_cocoa.modal)
        return;
    au_bw_close_tab(w->bw, index);
    au_cocoa_kick();
}

- (void)mouseDown:(NSEvent *)e
{
    AuCocoaWin *w = strip->win;
    if (!w || w->freed || au_cocoa.modal)
        return;
    if (e.modifierFlags & NSEventModifierFlagControl) {
        [self rightMouseDown:e];
        return;
    }
    if (index != au_bw_active_index(w->bw))
        au_bw_select_tab(w->bw, index);
    au_cocoa_kick();
    /* A drag moves the tab to where it is let go. */
    NSPoint start = [strip convertPoint:e.locationInWindow fromView:nil];
    bool dragged = false;
    for (;;) {
        NSEvent *next = [self.window nextEventMatchingMask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp];
        NSPoint at = [strip convertPoint:next.locationInWindow fromView:nil];
        if (fabs(at.x - start.x) + fabs(at.y - start.y) > 6)
            dragged = true;
        if (next.type == NSEventTypeLeftMouseUp) {
            if (dragged) {
                int to = [strip indexAt:at];
                if (to >= 0 && to != index && !w->freed)
                    au_bw_move_tab(w->bw, index, to);
                au_cocoa_win_changed(w, AU_CHANGED_TABS);
            }
            break;
        }
    }
}

- (void)otherMouseUp:(NSEvent *)e
{
    if (e.buttonNumber == 2)
        [self closeTab:nil];
}

/* The tab's own menu: the tab commands of the model, run on this tab. */
- (void)rightMouseDown:(NSEvent *)e
{
    AuCocoaWin *w = strip->win;
    if (!w || w->freed || au_cocoa.modal)
        return;
    if (index != au_bw_active_index(w->bw))
        au_bw_select_tab(w->bw, index);
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    m.autoenablesItems = NO;
    struct {
        NSString *title;
        SEL action;
        bool enabled;
    } rows[] = {
        {@"New Tab", @selector(newTab:), true},
        {@"Duplicate Tab", @selector(duplicateTab:), true},
        {@"Reload Tab", @selector(reloadTab:), true},
        {@"Unload Tab", @selector(unloadTab:), au_bw_can_unload_tab(w->bw, index)},
        {nil, NULL, false},
        {@"Close Tab", @selector(closeTab:), true},
        {@"Close Other Tabs", @selector(closeOthers:), au_bw_tab_count(w->bw) > 1},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (!rows[i].title) {
            [m addItem:NSMenuItem.separatorItem];
            continue;
        }
        NSMenuItem *it = [m addItemWithTitle:rows[i].title action:rows[i].action keyEquivalent:@""];
        it.target = self;
        it.enabled = rows[i].enabled;
    }
    [NSMenu popUpContextMenu:m withEvent:e forView:self];
}

- (void)newTab:(id)s
{
    (void)s;
    if (!strip->win->freed)
        au_bw_open_new_tab(strip->win->bw);
    au_cocoa_kick();
}

- (void)duplicateTab:(id)s
{
    (void)s;
    if (!strip->win->freed)
        au_bw_duplicate_tab(strip->win->bw, index);
    au_cocoa_kick();
}

- (void)reloadTab:(id)s
{
    (void)s;
    if (!strip->win->freed)
        au_bw_reload_tab(strip->win->bw, index);
    au_cocoa_kick();
}

- (void)unloadTab:(id)s
{
    (void)s;
    if (!strip->win->freed)
        au_bw_unload_tab(strip->win->bw, index);
    au_cocoa_kick();
}

- (void)closeOthers:(id)s
{
    (void)s;
    if (!strip->win->freed)
        au_bw_close_other_tabs(strip->win->bw, index);
    au_cocoa_kick();
}

@end

@implementation AuTabStrip

- (instancetype)initWithFrame:(NSRect)r
{
    self = [super initWithFrame:r];
    items = [NSMutableArray array];
    material = [[NSVisualEffectView alloc] initWithFrame:self.bounds];
    material.material = NSVisualEffectMaterialTitlebar;
    material.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    material.state = NSVisualEffectStateFollowsWindowActiveState;
    [self addSubview:material];
    edge = [[AuFillView alloc] initWithFill:NSColor.separatorColor];
    [self addSubview:edge];
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (NSRect)frameOf:(int)i count:(int)n
{
    NSRect b = self.bounds;
    if (vertical)
        return NSMakeRect(4, 4 + i * TAB_ROW_HEIGHT, b.size.width - 8, TAB_ROW_HEIGHT);
    CGFloat avail = b.size.width - 8;
    CGFloat w = n ? floor(avail / n) : TAB_MAX_WIDTH;
    if (w > TAB_MAX_WIDTH)
        w = TAB_MAX_WIDTH;
    if (w < TAB_MIN_WIDTH)
        w = TAB_MIN_WIDTH;
    return NSMakeRect(4 + i * w, 1, w, TAB_ROW_HEIGHT);
}

- (int)indexAt:(NSPoint)p
{
    int n = (int)items.count;
    for (int i = 0; i < n; i++) {
        NSRect f = [self frameOf:i count:n];
        if (vertical ? p.y < NSMaxY(f) : p.x < NSMaxX(f))
            return i;
    }
    return n - 1;
}

- (void)layout
{
    [super layout];
    NSRect b = self.bounds;
    material.frame = b;
    material.material = vertical ? NSVisualEffectMaterialSidebar : NSVisualEffectMaterialTitlebar;
    edge.frame = vertical ? NSMakeRect(NSMaxX(b) - 1, 0, 1, b.size.height) : NSMakeRect(0, NSMaxY(b) - 1, b.size.width, 1);
    int n = (int)items.count;
    for (int i = 0; i < n; i++)
        items[(NSUInteger)i].frame = [self frameOf:i count:n];
}

- (void)syncFrom:(AuCocoaWin *)w
{
    int n = au_bw_tab_count(w->bw);
    while ((int)items.count > n) {
        [items.lastObject removeFromSuperview];
        [items removeLastObject];
    }
    while ((int)items.count < n) {
        AuTabItem *it = [[AuTabItem alloc] initWithStrip:self];
        [items addObject:it];
        [self addSubview:it];
    }
    int active = au_bw_active_index(w->bw);
    for (int i = 0; i < n; i++) {
        AuTabItem *it = items[(NSUInteger)i];
        it->index = i;
        it->active = i == active;
        it->divider = i != active && i + 1 != active && i + 1 < n;
        it->close.hidden = !it->active && !it->hover;
        AuChromeTab t;
        if (!au_bw_tab_info(w->bw, i, &t))
            continue;
        const char *title = t.title && *t.title ? t.title : t.url && *t.url ? t.url : "Untitled";
        it->label.stringValue = au_cocoa_str(title);
        it->label.textColor = t.unloaded ? NSColor.tertiaryLabelColor
                              : it->active ? NSColor.labelColor
                                           : NSColor.secondaryLabelColor;
        it.toolTip = au_cocoa_str(t.url && *t.url ? t.url : title);
        NSImage *ic = icon_image(t.icon);
        it->icon.image = ic ? ic : symbol(@"globe", @"Page");
        it->icon.contentTintColor = ic ? nil : NSColor.secondaryLabelColor;
        it->icon.hidden = t.loading;
        if (t.loading)
            [it->spinner startAnimation:nil];
        else
            [it->spinner stopAnimation:nil];
        it.needsDisplay = YES;
    }
    self.needsLayout = YES;
}

@end

/* ---- The body ----
 * Lays out the tab strip (a row on top, or a column on the left), the find
 * bar, the page and its scroll bars, and the status bubble over the
 * page, by hand. */

@interface AuBodyView : NSView {
  @public
    __unsafe_unretained AuCocoaWin *win;
}
@end

@implementation AuBodyView

- (BOOL)isFlipped
{
    return YES;
}

- (void)layout
{
    [super layout];
    AuCocoaWin *w = win;
    if (!w)
        return;
    NSRect b = self.bounds;
    CGFloat W = b.size.width, H = b.size.height;
    CGFloat x0 = 0, y0 = 0;
    const AuPrefs *p = au_cocoa.browser ? &au_cocoa.browser->prefs : NULL;
    bool left = p && p->tab_position == AU_TABS_LEFT;
    w->strip->vertical = left;
    CGFloat bottom = H;
    if (left) {
        CGFloat cw = p->tab_column_width >= 100 ? p->tab_column_width : 200;
        if (cw > W / 2)
            cw = floor(W / 2);
        w->strip.frame = NSMakeRect(0, 0, cw, bottom);
        x0 = cw;
    } else {
        w->strip.frame = NSMakeRect(0, 0, W, STRIP_HEIGHT);
        y0 = STRIP_HEIGHT;
    }
    w->findBar.hidden = !w->find_shown;
    if (w->find_shown) {
        w->findBar.frame = NSMakeRect(x0, y0, W - x0, FIND_HEIGHT);
        y0 += FIND_HEIGHT;
    }
    CGFloat sw = [NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy];
    CGFloat hs = w->hscroll.hidden ? 0 : sw;
    CGFloat ph = bottom - y0 - hs, pwid = W - x0 - sw;
    if (ph < 1)
        ph = 1;
    if (pwid < 1)
        pwid = 1;
    w->page.frame = NSMakeRect(x0, y0, pwid, ph);
    w->vscroll.frame = NSMakeRect(x0 + pwid, y0, sw, ph);
    w->hscroll.frame = NSMakeRect(x0, y0 + ph, pwid, sw);
    /* The status bubble: over the page's bottom left corner, as wide as
     * its text and at most two thirds of the page. */
    NSSize ts = w->status.intrinsicContentSize;
    CGFloat bw = ceil(ts.width) + 16, maxw = floor(pwid * 2 / 3);
    if (bw > maxw)
        bw = maxw;
    w->statusBubble.frame = NSMakeRect(x0 - 1, y0 + ph - STATUS_HEIGHT + 1, bw, STATUS_HEIGHT);
    w->status.frame = NSMakeRect(8, 3, bw - 16, STATUS_HEIGHT - 6);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [NSColor.windowBackgroundColor setFill];
    NSRectFill(self.bounds);
}

@end

/* ---- The page surface ---- */

static void free_surface(AuCocoaWin *w)
{
    if (w->surf)
        au_surface_free(w->surf);
    free(w->pixels);
    w->surf = NULL;
    w->pixels = NULL;
}

/* A new surface for the page view's size, in points (docs/cocoa.md,
 * "HiDPI"), and the model laid out for it. */
void au_cocoa_win_page_resized(AuCocoaWin *w)
{
    NSSize s = w->page.bounds.size;
    int width = (int)s.width, height = (int)s.height;
    if (width < 1 || height < 1 || w->freed || (width == w->pw && height == w->ph && w->surf))
        return;
    free_surface(w);
    size_t stride = (size_t)width * 4;
    w->pixels = malloc(stride * (size_t)height);
    if (!w->pixels)
        return;
    memset(w->pixels, 0xFF, stride * (size_t)height);
    w->surf = au_surface_wrap(w->pixels, width, height, (int)stride);
    w->pw = width;
    w->ph = height;
    au_present_init(&w->present); /* a pending move was for the old surface */
    au_bw_resize(w->bw, width, height);
    au_present_invalidate(&w->present, (AuIRect){0, 0, width, height});
    w->pending |= AU_CHANGED_SCROLL;
    au_cocoa_kick();
}

/* The surface's pixels become the page layer's contents. The engine's
 * 0xAARRGGBB words are BGRA in memory on a little-endian Mac. */
static void show_surface(AuCocoaWin *w)
{
    size_t stride = (size_t)w->pw * 4;
    NSData *d = [NSData dataWithBytes:w->pixels length:stride * (size_t)w->ph];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)d);
    CGImageRef img = CGImageCreate((size_t)w->pw, (size_t)w->ph, 8, 32, stride, cs,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img)
        return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    w->page.layer.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
}

/* ---- Tabs ---- */

static void sync_tabs(AuCocoaWin *w)
{
    [w->strip syncFrom:w];
}

/* ---- Navigation ---- */

static void sync_nav(AuCocoaWin *w)
{
    w->back.enabled = au_bw_can_go(w->bw, -1);
    w->forward.enabled = au_bw_can_go(w->bw, 1);
    bool loading = au_bw_loading(w->bw);
    w->reload.image = loading ? symbol(@"xmark", @"Stop") : symbol(@"arrow.clockwise", @"Reload");
    w->reload.toolTip = loading ? @"Stop" : @"Reload";
}

static void sync_progress(AuCocoaWin *w)
{
    AuAddressField *f = (AuAddressField *)w->address;
    bool loading = au_bw_loading(w->bw);
    float p = loading ? au_bw_progress(w->bw) : 0;
    if (loading && p < 0.05f)
        p = 0.05f; /* a load that has started shows at once */
    if (p != f->progress) {
        f->progress = p;
        f.needsLayout = YES;
    }
}

static void sync_status(AuCocoaWin *w)
{
    char s[512];
    au_bw_status_text(w->bw, s, sizeof s);
    NSString *str = au_cocoa_str(s);
    w->statusBubble.hidden = !str.length;
    if (![str isEqualToString:w->status.stringValue]) {
        w->status.stringValue = str;
        w->body.needsLayout = YES;
    }
}

/* ---- The address field ----
 * Typing sets the model's field text (au_bw_set_field_text). When the text
 * grows at its end, the completion from history and bookmarks
 * (au_bw_complete_field) fills the rest, selected, so the next key
 * replaces it. */

static void complete_later(AuCocoaWin *w)
{
    __weak AuCocoaWin *weak = w;
    dispatch_async(dispatch_get_main_queue(), ^{
        AuCocoaWin *x = weak;
        if (!x || x->freed || !x->typed.length)
            return;
        NSText *ed = x->address.currentEditor;
        if (!ed || ![ed.string isEqualToString:x->typed])
            return;
        AuBuf out;
        au_buf_init(&out);
        const char *typed = x->typed.UTF8String;
        size_t tl = strlen(typed);
        if (au_bw_complete_field(x->bw, typed, &out) && out.len > tl && !strncmp(out.data, typed, tl)) {
            NSString *full = au_cocoa_str(au_buf_cstr(&out));
            x->syncing = true;
            ed.string = full;
            ed.selectedRange = NSMakeRange(x->typed.length, full.length - x->typed.length);
            x->syncing = false;
            au_bw_set_field_text(x->bw, full.UTF8String);
        }
        au_buf_free(&out);
    });
}

static bool address_focused(AuCocoaWin *w)
{
    NSResponder *fr = w->window.firstResponder;
    return [fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).delegate == (id)w->address;
}

static void sync_field(AuCocoaWin *w)
{
    /* Leave the text alone while the user edits it. */
    if (w->field_focus && w->typed && au_bw_typing(w->bw))
        return;
    const char *text = au_bw_field_text(w->bw);
    NSString *s = au_cocoa_str(text ? text : "");
    if ([s isEqualToString:w->address.stringValue])
        return;
    w->syncing = true;
    w->address.stringValue = s;
    w->syncing = false;
    w->typed = nil;
}

static void sync_security(AuCocoaWin *w)
{
    NSString *name = nil, *tip = nil;
    NSColor *tint = NSColor.secondaryLabelColor;
    switch (au_bw_security(w->bw)) {
    case AU_SECURITY_SECURE: name = @"lock.fill"; tip = @"Secure connection"; break;
    case AU_SECURITY_NOT_SECURE: name = @"lock.open"; tip = @"Not secure"; break;
    case AU_SECURITY_CERT_ERROR:
        name = @"exclamationmark.triangle.fill";
        tip = @"Certificate problem";
        tint = NSColor.systemRedColor;
        break;
    case AU_SECURITY_MIXED: name = @"lock.trianglebadge.exclamationmark"; tip = @"Partly secure"; break;
    default: break;
    }
    w->security.image = name ? symbol(name, tip) : nil;
    w->security.contentTintColor = tint;
    w->security.toolTip = tip;
}

void au_cocoa_win_focus_address(AuCocoaWin *w)
{
    [w->window makeFirstResponder:w->address];
    [w->address selectText:nil];
}

/* ---- Scroll bars ---- */

static void sync_scroller(NSScroller *s, int pos, int max, int page)
{
    if (page < 1)
        page = 1;
    s.enabled = max > 0;
    s.knobProportion = max > 0 ? (CGFloat)page / (CGFloat)(max + page) : 1.0;
    s.doubleValue = max > 0 ? (double)pos / (double)max : 0;
}

static void sync_scroll(AuCocoaWin *w)
{
    int x, y, mx, my;
    au_bw_scroll_pos(w->bw, &x, &y);
    au_bw_scroll_max(w->bw, &mx, &my);
    sync_scroller(w->hscroll, x, mx, w->pw);
    sync_scroller(w->vscroll, y, my, w->ph);
    /* The vertical bar stays (the page width does not jump as pages
     * load); the horizontal one shows only when the page is wider. */
    bool show_h = mx > 0;
    if (w->hscroll.hidden == show_h) {
        w->hscroll.hidden = !show_h;
        w->body.needsLayout = YES;
    }
}

/* ---- Find ---- */

void au_cocoa_win_find(AuCocoaWin *w)
{
    if (!w->find_shown) {
        w->find_shown = true;
        w->body.needsLayout = YES;
    }
    [w->window makeFirstResponder:w->findField];
    [w->findField selectText:nil];
}

static void find_hide(AuCocoaWin *w)
{
    w->find_shown = false;
    w->body.needsLayout = YES;
    [w->window makeFirstResponder:w->page];
}

/* ---- The window ---- */

@implementation AuCocoaWin

static NSToolbarItemIdentifier const ID_BACK = @"aura.back";
static NSToolbarItemIdentifier const ID_FORWARD = @"aura.forward";
static NSToolbarItemIdentifier const ID_ADDRESS = @"aura.address";
static NSToolbarItemIdentifier const ID_NEWTAB = @"aura.newtab";

/* A toolbar button as AppKit draws them: borderless until the pointer is
 * over it. Its enabled state is set by sync_nav, not validated. */
- (NSToolbarItem *)toolItem:(NSToolbarItemIdentifier)ident
                     symbol:(NSString *)sym
                      label:(NSString *)label
                     action:(SEL)action
{
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    it.image = symbol(sym, label);
    it.label = label;
    it.toolTip = label;
    it.target = self;
    it.action = action;
    it.bordered = YES;
    it.autovalidates = NO;
    return it;
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)tb
{
    (void)tb;
    return @[ ID_BACK, ID_FORWARD, NSToolbarFlexibleSpaceItemIdentifier, ID_ADDRESS,
              NSToolbarFlexibleSpaceItemIdentifier, ID_NEWTAB ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)tb
{
    return [self toolbarDefaultItemIdentifiers:tb];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)tb itemForItemIdentifier:(NSToolbarItemIdentifier)ident
    willBeInsertedIntoToolbar:(BOOL)flag
{
    (void)tb;
    (void)flag;
    if ([ident isEqualToString:ID_BACK]) {
        back = [self toolItem:ident symbol:@"chevron.left" label:@"Back" action:@selector(goBack:)];
        back.navigational = YES;
        return back;
    }
    if ([ident isEqualToString:ID_FORWARD]) {
        forward = [self toolItem:ident symbol:@"chevron.right" label:@"Forward" action:@selector(goForward:)];
        forward.navigational = YES;
        return forward;
    }
    if ([ident isEqualToString:ID_NEWTAB])
        return addTab = [self toolItem:ident symbol:@"plus" label:@"New Tab" action:@selector(newTab:)];
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    if ([ident isEqualToString:ID_ADDRESS]) {
        /* 240 to 1000 points; the toolbar stretches it over the free
         * width by itself. A constraint that asks for more (even at a low
         * priority) makes the toolbar move every item into its overflow
         * menu. */
        [address.widthAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
        [address.widthAnchor constraintLessThanOrEqualToConstant:1000].active = YES;
        it.view = address;
        it.label = @"Address";
        it.visibilityPriority = NSToolbarItemVisibilityPriorityHigh;
    }
    return it;
}

- (void)goBack:(id)s
{
    (void)s;
    if (!au_cocoa.modal && !freed)
        au_bw_back(bw);
    au_cocoa_kick();
}

- (void)goForward:(id)s
{
    (void)s;
    if (!au_cocoa.modal && !freed)
        au_bw_forward(bw);
    au_cocoa_kick();
}

- (void)reloadOrStop:(id)s
{
    (void)s;
    if (au_cocoa.modal || freed)
        return;
    /* Shift or Option reloads past the cache, as Reload in the menu does. */
    au_bw_reload_or_stop(bw, (au_cocoa_current_mods() & (AU_MOD_SHIFT | AU_MOD_OPTION)) != 0);
    au_cocoa_kick();
}

- (void)newTab:(id)s
{
    (void)s;
    if (!au_cocoa.modal && !freed)
        au_bw_open_new_tab(bw);
    au_cocoa_kick();
}

- (void)scrolled:(NSScroller *)s
{
    if (syncing || freed || au_cocoa.modal)
        return;
    int x, y, mx, my;
    au_bw_scroll_pos(bw, &x, &y);
    au_bw_scroll_max(bw, &mx, &my);
    bool vert = s == vscroll;
    int pos = vert ? y : x, max = vert ? my : mx, extent = vert ? ph : pw;
    int step = extent > 80 ? extent - 40 : extent;
    switch (s.hitPart) {
    case NSScrollerKnob:
    case NSScrollerKnobSlot: pos = (int)(s.doubleValue * max + 0.5); break;
    case NSScrollerDecrementPage: pos -= step; break;
    case NSScrollerIncrementPage: pos += step; break;
    default: return;
    }
    if (pos < 0)
        pos = 0;
    if (pos > max)
        pos = max;
    au_bw_scroll_to(bw, vert ? x : pos, vert ? pos : y);
    au_cocoa_kick();
}

/* Address field and find field. */

- (void)controlTextDidChange:(NSNotification *)n
{
    if (n.object != address || syncing || freed)
        return;
    NSText *ed = address.currentEditor;
    NSString *text = ed ? ed.string : address.stringValue;
    bool grew = typed ? text.length > typed.length && [text hasPrefix:typed] : text.length > 0;
    bool at_end = ed && NSMaxRange(ed.selectedRange) >= text.length;
    typed = [text copy];
    au_bw_set_field_text(bw, text.UTF8String);
    if (grew && at_end)
        complete_later(self);
}

- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)cmd
{
    (void)tv;
    if (c == address) {
        if (cmd == @selector(insertNewline:)) {
            if (au_cocoa.modal || freed)
                return YES;
            NSString *text = [address.stringValue copy];
            typed = nil;
            [window makeFirstResponder:page];
            au_bw_navigate_input(bw, text.UTF8String);
            au_cocoa_kick();
            return YES;
        }
        if (cmd == @selector(cancelOperation:)) {
            /* Escape puts back the page's address. */
            typed = nil;
            au_bw_set_field_text(bw, NULL);
            au_cocoa_win_changed(self, AU_CHANGED_FIELD);
            return YES;
        }
        return NO;
    }
    if (c == findField) {
        if (cmd == @selector(insertNewline:)) {
            [self findNext:(NSApp.currentEvent.modifierFlags & NSEventModifierFlagShift) == 0];
            return YES;
        }
        if (cmd == @selector(cancelOperation:)) {
            find_hide(self);
            return YES;
        }
    }
    return NO;
}

- (void)findNext:(bool)fwd
{
    NSString *text = findField.stringValue;
    if (!text.length || au_cocoa.modal || freed)
        return;
    static NSString *last;
    bool ok = [text isEqualToString:last] ? au_bw_find_again(bw, fwd) : au_bw_find(bw, text.UTF8String, false);
    last = [text copy];
    if (!ok)
        NSBeep();
    au_cocoa_kick();
}

- (void)closeFind:(id)s
{
    (void)s;
    find_hide(self);
}

/* Window events. */

- (void)windowDidBecomeKey:(NSNotification *)n
{
    (void)n;
    if (freed)
        return;
    au_bw_set_active(bw, true);
    au_cocoa.front = self;
    au_cocoa_menus_dirty();
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    if (!freed)
        au_bw_set_active(bw, false);
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    if (au_cocoa.modal)
        return NO;
    /* The window closes with its tabs; Aura keeps running, as Mac
     * applications do. */
    au_cocoa_win_destroy(self);
    return NO;
}

- (void)observeValueForKeyPath:(NSString *)path ofObject:(id)obj change:(NSDictionary *)change context:(void *)ctx
{
    (void)path;
    (void)obj;
    (void)change;
    (void)ctx;
    if (freed)
        return;
    bool f = address_focused(self);
    if (f != field_focus) {
        field_focus = f;
        au_bw_set_field_focus(bw, f);
    }
}

@end

AuCocoaWin *au_cocoa_win_new(int width, int height)
{
    AuCocoaWin *w = [AuCocoaWin new];
    w->bw = au_browser_window_new_native(au_cocoa.browser, width, height);
    if (!w->bw)
        return nil;
    w->bw->platform = (__bridge void *)w;
    w->bw->native_field = true;
    au_present_init(&w->present);
    if (!wins)
        wins = [NSMutableArray array];

    NSRect frame = NSMakeRect(0, 0, width, height + STRIP_HEIGHT);
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:NO];
    w->window = win;
    win.releasedWhenClosed = NO;
    win.title = @"Aura";
    win.titleVisibility = NSWindowTitleHidden;
    win.toolbarStyle = NSWindowToolbarStyleUnified;
    /* The tab strip draws the line under the toolbar and the strip. */
    win.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    win.tabbingMode = NSWindowTabbingModeDisallowed; /* Aura's own tabs */
    win.delegate = w;
    win.minSize = NSMakeSize(420, 300);

    AuAddressField *af = [AuAddressField new];
    w->address = af;
    w->security = af->icon = [NSImageView new];
    w->security.imageScaling = NSImageScaleProportionallyDown;
    w->reload = af->reload = [NSButton buttonWithImage:symbol(@"arrow.clockwise", @"Reload")
                                                target:w
                                                action:@selector(reloadOrStop:)];
    w->reload.bordered = NO;
    w->reload.imageScaling = NSImageScaleProportionallyDown;
    w->reload.contentTintColor = NSColor.secondaryLabelColor;
    w->reload.toolTip = @"Reload";
    w->reload.refusesFirstResponder = YES;
    w->loadBar = af->bar = [[AuFillView alloc] initWithFill:NSColor.controlAccentColor];
    af->bar.radius = 1;
    af->bar.hidden = YES;
    for (NSView *v in @[ w->security, w->reload, af->bar ])
        [af addSubview:v];
    w->address.controlSize = NSControlSizeLarge;
    w->address.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    w->address.translatesAutoresizingMaskIntoConstraints = NO;
    w->address.placeholderString = @"Search or enter address";
    w->address.delegate = w;
    w->address.bezelStyle = NSTextFieldRoundedBezel;
    w->address.cell.scrollable = YES;
    w->address.cell.wraps = NO;
    w->address.lineBreakMode = NSLineBreakByTruncatingTail;
    w->address.usesSingleLineMode = YES;
    w->address.automaticTextCompletionEnabled = NO;

    NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:@"aura.browser"];
    tb.delegate = w;
    tb.displayMode = NSToolbarDisplayModeIconOnly;
    tb.allowsUserCustomization = NO;
    win.toolbar = tb;

    AuBodyView *body = [[AuBodyView alloc] initWithFrame:frame];
    body->win = w;
    w->body = body;

    w->strip = [[AuTabStrip alloc] initWithFrame:NSMakeRect(0, 0, width, STRIP_HEIGHT)];
    w->strip->win = w;
    [body addSubview:w->strip];

    w->findBar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, FIND_HEIGHT)];
    w->findField = [NSSearchField new];
    w->findField.placeholderString = @"Find in page";
    w->findField.delegate = w;
    w->findField.sendsWholeSearchString = YES;
    w->findField.frame = NSMakeRect(8, 4, 300, 24);
    NSButton *done = [NSButton buttonWithTitle:@"Done" target:w action:@selector(closeFind:)];
    done.controlSize = NSControlSizeSmall;
    done.frame = NSMakeRect(316, 6, 64, 20);
    [w->findBar addSubview:w->findField];
    [w->findBar addSubview:done];
    w->findBar.hidden = YES;
    [body addSubview:w->findBar];

    w->page = [[AuPageView alloc] initWithFrame:NSMakeRect(0, STRIP_HEIGHT, width, height)];
    w->page->win = w;
    [body addSubview:w->page];

    w->vscroll = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, 15, 100)];
    w->hscroll = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, 100, 15)];
    for (NSScroller *s in @[ w->vscroll, w->hscroll ]) {
        s.scrollerStyle = NSScrollerStyleLegacy;
        s.target = w;
        s.action = @selector(scrolled:);
        s.enabled = NO;
        [body addSubview:s];
    }
    w->hscroll.hidden = YES;

    AuFillView *bubble = [[AuFillView alloc] initWithFill:NSColor.windowBackgroundColor];
    bubble.stroke = NSColor.separatorColor;
    bubble.radius = 5;
    bubble.hidden = YES;
    w->statusBubble = bubble;
    w->status = [NSTextField labelWithString:@""];
    w->status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    w->status.textColor = NSColor.secondaryLabelColor;
    w->status.lineBreakMode = NSLineBreakByTruncatingMiddle;
    w->status.cell.usesSingleLineMode = YES;
    [bubble addSubview:w->status];
    [body addSubview:bubble]; /* last: over the page */

    /* Below the toolbar: the content view is the whole window
     * (FullSizeContentView), so the body sits in the layout guide. */
    NSView *content = [[NSView alloc] initWithFrame:frame];
    win.contentView = content;
    body.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:body];
    NSLayoutGuide *g = win.contentLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [body.topAnchor constraintEqualToAnchor:g.topAnchor],
        [body.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [body.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [body.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
    ]];
    [win addObserver:w forKeyPath:@"firstResponder" options:0 context:NULL];

    /* New windows cascade from the front one. */
    AuCocoaWin *front = au_cocoa_front();
    if (front)
        [win setFrameTopLeftPoint:[win cascadeTopLeftFromPoint:NSMakePoint(NSMinX(front->window.frame),
                                                                          NSMaxY(front->window.frame))]];
    else
        [win center];

    [wins insertObject:w atIndex:0];
    au_cocoa.front = w;
    w->pending = ~0u & ~(unsigned)(AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE);
    au_cocoa_menus_dirty();
    [win makeFirstResponder:w->page];
    return w;
}

void au_cocoa_win_destroy(AuCocoaWin *w)
{
    if (w->freed)
        return;
    [wins removeObject:w];
    if (au_cocoa.front == w)
        au_cocoa.front = nil;
    if (w->have_menu)
        au_context_menu_free(&w->menu);
    w->have_menu = false;
    AuBrowserWindow *bw = w->bw;
    w->freed = true;
    w->bw = NULL;
    bw->platform = NULL;
    au_browser_window_free(bw);
    free_surface(w);
    [w->window removeObserver:w forKeyPath:@"firstResponder"];
    w->window.delegate = nil;
    w->strip->win = nil;
    ((AuBodyView *)w->body)->win = nil;
    w->page->win = nil;
    [w->window orderOut:nil];
    [w->window close];
    au_cocoa_menus_dirty();
}

void au_cocoa_win_changed(AuCocoaWin *w, unsigned what)
{
    /* The later of two focus requests in one batch wins: a new tab asks
     * for the field while blank, then for the page once its load starts. */
    if (what & (AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE))
        w->pending &= ~(unsigned)(AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE);
    w->pending |= what;
    au_cocoa_kick();
}

static void apply_changes(AuCocoaWin *w)
{
    unsigned what = w->pending;
    w->pending = 0;
    if (!what)
        return;
    if (what & AU_CHANGED_TITLE) {
        char title[256];
        au_bw_window_title(w->bw, title, sizeof title);
        w->window.title = *title ? au_cocoa_str(title) : @"Aura";
        au_cocoa_menus_dirty(); /* the Window menu lists the titles */
    }
    if (what & AU_CHANGED_LAYOUT)
        w->body.needsLayout = YES;
    if (what & (AU_CHANGED_TABS | AU_CHANGED_TAB_INFO | AU_CHANGED_ACTIVE_TAB | AU_CHANGED_LAYOUT)) {
        w->syncing = true;
        sync_tabs(w);
        w->syncing = false;
    }
    if (what & (AU_CHANGED_NAV | AU_CHANGED_ACTIVE_TAB | AU_CHANGED_TABS))
        sync_nav(w);
    if (what & (AU_CHANGED_FIELD | AU_CHANGED_ACTIVE_TAB))
        sync_field(w);
    if (what & (AU_CHANGED_SECURITY | AU_CHANGED_ACTIVE_TAB))
        sync_security(w);
    if (what & (AU_CHANGED_SCROLL | AU_CHANGED_LAYOUT | AU_CHANGED_ACTIVE_TAB)) {
        w->syncing = true;
        sync_scroll(w);
        w->syncing = false;
    }
    if (what & (AU_CHANGED_STATUS | AU_CHANGED_ACTIVE_TAB))
        sync_status(w);
    if (what & (AU_CHANGED_PROGRESS | AU_CHANGED_NAV | AU_CHANGED_ACTIVE_TAB))
        sync_progress(w);
    if (what & (AU_CHANGED_MENUS | AU_CHANGED_BOOKMARK))
        au_cocoa_menus_dirty();
    if (what & AU_CHANGED_FOCUS_FIELD)
        au_cocoa_win_focus_address(w);
    else if (what & AU_CHANGED_FOCUS_PAGE)
        [w->window makeFirstResponder:w->page];
}

void au_cocoa_win_flush(AuCocoaWin *w)
{
    if (w->freed)
        return;
    apply_changes(w);
    if (!au_present_pending(&w->present) || !w->surf)
        return;
    AuPresentRound r;
    au_present_begin(&w->present, w->surf, &r);
    AuCanvas *c = au_canvas_new(w->surf);
    for (int i = 0; i < r.n; i++)
        au_bw_paint(w->bw, c, r.rects[i]);
    au_canvas_free(c);
    show_surface(w);
}
