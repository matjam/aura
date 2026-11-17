/* The page view: the model's surface as its layer's contents, and input
 * on the page (docs/cocoa.md, "Input"). Each event becomes an AuEvent for
 * au_bw_key or au_bw_mouse, in the page surface's points.
 *
 * Modifiers follow the Mac, as on Mac OS 9: Command is AU_MOD_COMMAND
 * (Command-click opens a link in a new tab), Option is AU_MOD_OPTION
 * (Option with an arrow or Delete works by word in a text field), and
 * Control-click opens the contextual menu.
 *
 * Text comes through NSTextInputClient, so dead keys, the accent menu and
 * input methods work; each inserted code point is one AU_EVENT_KEY_DOWN. */
#import "cocoa.h"

#import <Carbon/Carbon.h> /* kVK_* key codes */
#import <QuartzCore/QuartzCore.h>

#include <string.h>

uint32_t au_cocoa_mods(NSEventModifierFlags f)
{
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift)
        m |= AU_MOD_SHIFT;
    if (f & NSEventModifierFlagCommand)
        m |= AU_MOD_COMMAND;
    if (f & NSEventModifierFlagOption)
        m |= AU_MOD_OPTION;
    return m;
}

uint32_t au_cocoa_current_mods(void)
{
    return au_cocoa_mods(NSEvent.modifierFlags);
}

static void send_key(AuCocoaWin *w, int key, uint32_t ch, uint32_t mods, bool *handled)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.time_ms = au_posix_ms();
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    bool h = au_bw_key(w->bw, &e);
    if (!h && key == AU_KEY_ESCAPE) {
        au_bw_stop(w->bw); /* Escape the page did not take stops the load */
        h = true;
    }
    if (handled)
        *handled = h;
    au_cocoa_kick();
}

static int special_key(unsigned short code)
{
    switch (code) {
    case kVK_LeftArrow: return AU_KEY_LEFT;
    case kVK_RightArrow: return AU_KEY_RIGHT;
    case kVK_UpArrow: return AU_KEY_UP;
    case kVK_DownArrow: return AU_KEY_DOWN;
    case kVK_PageUp: return AU_KEY_PAGE_UP;
    case kVK_PageDown: return AU_KEY_PAGE_DOWN;
    case kVK_Home: return AU_KEY_HOME;
    case kVK_End: return AU_KEY_END;
    case kVK_Delete: return AU_KEY_BACKSPACE;
    case kVK_ForwardDelete: return AU_KEY_DELETE;
    case kVK_Return:
    case kVK_ANSI_KeypadEnter: return AU_KEY_RETURN;
    case kVK_Tab: return AU_KEY_TAB;
    case kVK_Escape: return AU_KEY_ESCAPE;
    default: return AU_KEY_NONE;
    }
}

/* A wheel step scrolls 48 points (three 16-point lines, as the arrow keys
 * move); trackpads scroll by the points they report. */
#define WHEEL_STEP 48

@implementation AuPageView {
    NSTrackingArea *tracking;
    NSString *marked; /* the input method's composition, not yet inserted */
    double scroll_rx, scroll_ry; /* trackpad scrolling's fractions of a pixel, carried to the next event */
}

- (instancetype)initWithFrame:(NSRect)r
{
    self = [super initWithFrame:r];
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    self.layer.backgroundColor = NSColor.whiteColor.CGColor;
    /* Points enlarged to the screen's pixels as they are (docs/cocoa.md,
     * "HiDPI"). */
    self.layer.magnificationFilter = kCAFilterNearest;
    self.layer.contentsGravity = kCAGravityResize;
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)e
{
    (void)e;
    return YES;
}

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (void)updateLayer
{
}

- (void)setFrameSize:(NSSize)s
{
    [super setFrameSize:s];
    if (win && !win->freed)
        au_cocoa_win_page_resized(win);
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (tracking)
        [self removeTrackingArea:tracking];
    tracking = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking];
}

- (bool)live
{
    return win && !win->freed && !au_cocoa.modal;
}

/* ---- Keys ---- */

- (void)keyDown:(NSEvent *)e
{
    AuCocoaWin *w = win;
    if (![self live])
        return;
    NSEventModifierFlags f = e.modifierFlags;
    /* Command-1 to Command-8 choose a tab, Command-9 the last. */
    if ((f & NSEventModifierFlagCommand) && !(f & (NSEventModifierFlagOption | NSEventModifierFlagShift |
                                                    NSEventModifierFlagControl))) {
        NSString *c = e.charactersIgnoringModifiers;
        unichar ch = c.length ? [c characterAtIndex:0] : 0;
        if (ch >= '1' && ch <= '9') {
            int n = au_bw_tab_count(w->bw);
            int i = ch == '9' ? n - 1 : ch - '1';
            if (i >= 0 && i < n)
                au_bw_select_tab(w->bw, i);
            au_cocoa_kick();
            return;
        }
    }
    int key = special_key(e.keyCode);
    if (key != AU_KEY_NONE && !marked.length) {
        bool handled;
        send_key(w, key, 0, au_cocoa_mods(f), &handled);
        /* Tab the page did not take stays on the page rather than moving
         * the keyboard focus to the toolbar. */
        if (!handled && key != AU_KEY_TAB && key != AU_KEY_ESCAPE)
            [super keyDown:e];
        return;
    }
    if (f & NSEventModifierFlagCommand) {
        /* Not a menu shortcut (the menu bar had it first): nothing. */
        [super keyDown:e];
        return;
    }
    [self interpretKeyEvents:@[ e ]];
}

/* Commands from the text system for keys handled above; nothing more. */
- (void)doCommandBySelector:(SEL)sel
{
    (void)sel;
}

/* ---- NSTextInputClient ---- */

- (void)insertText:(id)string replacementRange:(NSRange)range
{
    (void)range;
    AuCocoaWin *w = win;
    marked = nil;
    if (![self live])
        return;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    uint32_t mods = au_cocoa_current_mods() & AU_MOD_SHIFT;
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *sub, NSRange r, NSRange er, BOOL *stop) {
                           (void)r;
                           (void)er;
                           (void)stop;
                           /* Each code point of the cluster. */
                           NSUInteger n = sub.length;
                           for (NSUInteger k = 0; k < n; k++) {
                               uint32_t c = [sub characterAtIndex:k];
                               if (CFStringIsSurrogateHighCharacter((UniChar)c) && k + 1 < n) {
                                   c = (uint32_t)CFStringGetLongCharacterForSurrogatePair(
                                       (UniChar)c, [sub characterAtIndex:k + 1]);
                                   k++;
                               }
                               if (c == '\n' || c == '\r')
                                   send_key(w, AU_KEY_RETURN, 0, mods, NULL);
                               else if (c == '\t')
                                   send_key(w, AU_KEY_TAB, 0, mods, NULL);
                               else if (c >= 0x20 && c != 0x7F && !(c >= 0xF700 && c <= 0xF8FF))
                                   send_key(w, 0, c, mods, NULL);
                           }
                       }];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep
{
    (void)sel;
    (void)rep;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    marked = s.length ? [s copy] : nil;
}

- (void)unmarkText
{
    marked = nil;
}

- (NSRange)selectedRange
{
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange
{
    return marked.length ? NSMakeRange(0, marked.length) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText
{
    return marked.length > 0;
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)actual
{
    (void)r;
    (void)actual;
    return nil;
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
    return @[];
}

/* The input method's window goes near the pointer's last place on the
 * page; the model does not report its caret. */
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)actual
{
    (void)r;
    (void)actual;
    NSRect local = NSMakeRect(win ? win->last_x : 0, win ? win->last_y : 0, 1, 18);
    return [self.window convertRectToScreen:[self convertRect:local toView:nil]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)p
{
    (void)p;
    return NSNotFound;
}

/* ---- The mouse ---- */

- (NSPoint)pointOf:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (win) {
        win->last_x = p.x;
        win->last_y = p.y;
    }
    return p;
}

- (void)sendMouse:(AuEventType)type at:(NSPoint)p button:(int)button mods:(uint32_t)mods
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.time_ms = au_posix_ms();
    e.x = (int)p.x;
    e.y = (int)p.y;
    e.button = button;
    e.clicks = win->clicks ? win->clicks : 1;
    e.mods = mods;
    au_bw_mouse(win->bw, &e);
    au_cocoa_kick();
}

- (void)mouseDown:(NSEvent *)e
{
    if (![self live])
        return;
    NSPoint p = [self pointOf:e];
    [self.window makeFirstResponder:self];
    if (e.modifierFlags & NSEventModifierFlagControl) {
        au_cocoa_context_menu(win, e, p.x, p.y);
        return;
    }
    win->clicks = (int)(e.clickCount > 3 ? ((e.clickCount - 1) % 3) + 1 : e.clickCount);
    win->pressed = true;
    win->click_mods = au_cocoa_mods(e.modifierFlags);
    [self sendMouse:AU_EVENT_MOUSE_DOWN at:p button:0 mods:win->click_mods];
}

- (void)mouseDragged:(NSEvent *)e
{
    if (![self live] || !win->pressed)
        return;
    [self sendMouse:AU_EVENT_MOUSE_MOVE at:[self pointOf:e] button:0 mods:au_cocoa_mods(e.modifierFlags)];
}

- (void)mouseUp:(NSEvent *)e
{
    if (![self live] || !win->pressed)
        return;
    win->pressed = false;
    [self sendMouse:AU_EVENT_MOUSE_UP
                 at:[self pointOf:e]
             button:0
               mods:win->click_mods | (au_cocoa_mods(e.modifierFlags) & AU_MOD_SHIFT)];
}

- (void)mouseMoved:(NSEvent *)e
{
    if (![self live] || win->pressed)
        return;
    [self sendMouse:AU_EVENT_MOUSE_MOVE at:[self pointOf:e] button:-1 mods:au_cocoa_mods(e.modifierFlags)];
}

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    if (![self live] || win->pressed)
        return;
    au_bw_mouse_leave(win->bw);
    au_cocoa_kick();
}

- (void)rightMouseDown:(NSEvent *)e
{
    if (![self live])
        return;
    NSPoint p = [self pointOf:e];
    [self.window makeFirstResponder:self];
    au_cocoa_context_menu(win, e, p.x, p.y);
}

/* The middle button clicks with Command (a link opens in a new tab); the
 * side buttons go Back and Forward. */
- (void)otherMouseDown:(NSEvent *)e
{
    if (![self live])
        return;
    NSPoint p = [self pointOf:e];
    switch (e.buttonNumber) {
    case 2:
        win->clicks = 1;
        win->pressed = true;
        win->click_mods = au_cocoa_mods(e.modifierFlags) | AU_MOD_COMMAND;
        [self sendMouse:AU_EVENT_MOUSE_DOWN at:p button:0 mods:win->click_mods];
        break;
    case 3: au_bw_back(win->bw); au_cocoa_kick(); break;
    case 4: au_bw_forward(win->bw); au_cocoa_kick(); break;
    default: break;
    }
}

- (void)otherMouseUp:(NSEvent *)e
{
    if (e.buttonNumber == 2)
        [self mouseUp:e];
}

- (void)otherMouseDragged:(NSEvent *)e
{
    if (e.buttonNumber == 2)
        [self mouseDragged:e];
}

- (void)scrollWheel:(NSEvent *)e
{
    if (![self live])
        return;
    NSPoint p = [self pointOf:e];
    double scale = e.hasPreciseScrollingDeltas ? 1 : WHEEL_STEP;
    double dx = -e.scrollingDeltaX * scale, dy = -e.scrollingDeltaY * scale;
    /* A trackpad sends fractions of a pixel; rounding each event alone
     * lost slow movements. Carry the remainder, and drop it when a
     * gesture begins. */
    if (e.phase == NSEventPhaseBegan || !e.hasPreciseScrollingDeltas)
        scroll_rx = scroll_ry = 0;
    dx += scroll_rx;
    dy += scroll_ry;
    double rdx = trunc(dx), rdy = trunc(dy);
    if (e.hasPreciseScrollingDeltas) {
        scroll_rx = dx - rdx;
        scroll_ry = dy - rdy;
    } else {
        rdx = round(dx);
        rdy = round(dy);
    }
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_SCROLL;
    ev.time_ms = au_posix_ms();
    ev.x = (int)p.x;
    ev.y = (int)p.y;
    ev.dx = (int)rdx;
    ev.dy = (int)rdy;
    ev.mods = au_cocoa_mods(e.modifierFlags) & ~(uint32_t)AU_MOD_SHIFT;
    if (!ev.dx && !ev.dy)
        return;
    au_bw_mouse(win->bw, &ev);
    au_cocoa_kick();
}

/* A page dragged in from the Finder or another program: open it. */
- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self registerForDraggedTypes:@[ NSPasteboardTypeURL, NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    (void)info;
    return [self live] ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    if (![self live])
        return NO;
    NSPasteboard *pb = info.draggingPasteboard;
    NSURL *u = [NSURL URLFromPasteboard:pb];
    NSString *s = u ? u.absoluteString : [pb stringForType:NSPasteboardTypeString];
    if (!s.length)
        return NO;
    au_bw_navigate_input(win->bw, s.UTF8String);
    au_cocoa_kick();
    return YES;
}

@end
