/* The developer windows: Page Source, Console, Network, Performance,
 * System Status and Report a Problem.
 *
 * The model (src/browser/devwin_model.h) holds the text, the parts of the
 * window and their rectangles, the buttons and toggles, the filter field
 * and the Console's input line. This file paints the model as the Mac OS 9
 * window does, with the engine's canvas into a pixel buffer the size of a
 * flipped NSView (the engine's premultiplied 0xAARRGGBB words are BGRA
 * bytes on a little-endian Mac; the buffer becomes the view layer's
 * contents as a CGImage), and places native controls over it at the
 * model's rectangles: the push buttons, the tool band's check boxes and
 * radio buttons, and an NSScroller for each text pane. The painted view
 * keeps the keyboard; the controls over it refuse first responder. The
 * painted parts are always light, so the window keeps the light (Aqua)
 * appearance and its controls read on them in Dark Mode too.
 *
 * The develop.windows preference keeps each window's size and, since
 * macOS lets a window choose its place, its position: the content's
 * top-left in points from the top-left of the main screen, as Mac OS 9
 * keeps global coordinates. A saved position that no screen shows any
 * more is dropped and the window is centred. */
#import "dialogs_cocoa.h"

#include <string.h>

#include "browser/chrome.h"

@class AuDevWinCtl;

/* ---- The painted view ---- */

/* Characters come through the text input system (NSTextInputClient), so
 * dead keys and input methods compose; the composing text is not drawn,
 * and the composed text is typed when it is committed. */
@interface AuDevView : NSView <NSTextInputClient> {
  @public
    __weak AuDevWinCtl *ctl;
    NSMutableData *pixels;
    int pw, ph;
    double wheel_acc; /* rows of precise scrolling not yet applied */
    AuDevWinPane wheel_pane;
    NSString *marked; /* the text being composed, or nil */
    NSEvent *key_event; /* the key being interpreted */
}
@end

/* ---- A window ---- */

@interface AuDevWinCtl : NSObject <NSWindowDelegate> {
  @public
    AuDevWin m;
    NSWindow *win;
    AuDevView *view;
    NSScroller *vscroll, *dscroll;
    NSButton *buttons[AU_DEVWIN_MAX_BUTTONS];
    NSButton *toggles[AU_DEVWIN_MAX_TOGGLES];
    int nbuttons, ntoggles;
    int footer_right; /* the last button's right edge, as placed */
    bool syncing;     /* the controls are being set from the model */
    bool closed;      /* the model is freed */
    NSTimer *timer;
    /* The mouse. */
    NSTimer *drag_timer;
    NSPoint mouse;
    AuDevWinPane drag_pane;
    bool dragging;
}
- (void)update;
- (void)resized;
- (void)paintInto:(AuDevView *)v;
- (void)mouseDown:(NSEvent *)e at:(NSPoint)p;
- (void)mouseDragged:(NSEvent *)e at:(NSPoint)p;
- (void)mouseUp:(NSEvent *)e;
- (void)wheel:(NSEvent *)e at:(NSPoint)p;
- (bool)key:(NSEvent *)e;
- (void)typeText:(NSString *)s;
- (void)typeKey:(NSEvent *)e;
@end

static AuDevWinCtl *g_dev[AU_DEVWIN_KINDS];
static AuTimingTest g_timing;
static const void *g_timing_page;
static AuPaintBench g_bench;
static int g_scroll_w; /* what each text pane leaves at its right for its scroll bar */

static AuPage *front_page(void)
{
    return au_cocoa_wh.front_page ? au_cocoa_wh.front_page(au_cocoa_wh.ctx) : NULL;
}

static NSWindow *front_ns_window(void)
{
    return au_cocoa_wh.front_ns_window ? au_cocoa_wh.front_ns_window(au_cocoa_wh.ctx) : nil;
}

static int scroll_w(void)
{
    if (!g_scroll_w)
        g_scroll_w = (int)ceil([NSScroller scrollerWidthForControlSize:NSControlSizeRegular
                                                          scrollerStyle:NSScrollerStyleLegacy]);
    return g_scroll_w;
}

@implementation AuDevView

- (instancetype)initWithFrame:(NSRect)r
{
    self = [super initWithFrame:r];
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    self.layer.backgroundColor = NSColor.whiteColor.CGColor;
    /* Points enlarged to the screen's pixels as they are, as the page is
     * (docs/cocoa.md, "HiDPI"). */
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
    [ctl paintInto:self];
}

- (void)setFrameSize:(NSSize)s
{
    [super setFrameSize:s];
    [ctl resized];
}

- (NSPoint)pointOf:(NSEvent *)e
{
    return [self convertPoint:e.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent *)e
{
    [ctl mouseDown:e at:[self pointOf:e]];
}

- (void)mouseDragged:(NSEvent *)e
{
    [ctl mouseDragged:e at:[self pointOf:e]];
}

- (void)mouseUp:(NSEvent *)e
{
    [ctl mouseUp:e];
}

- (void)scrollWheel:(NSEvent *)e
{
    [ctl wheel:e at:[self pointOf:e]];
}

- (void)keyDown:(NSEvent *)e
{
    if (![ctl key:e])
        [super keyDown:e];
}

/* ---- NSTextInputClient ---- */

- (void)insertText:(id)string replacementRange:(NSRange)range
{
    (void)range;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    marked = nil;
    if (s.length)
        [ctl typeText:s];
}

- (void)doCommandBySelector:(SEL)sel
{
    /* A key the input system did not take (an arrow after a dead key):
     * the window's own key handling. */
    (void)sel;
    if (key_event)
        [ctl typeKey:key_event];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range
{
    (void)sel;
    (void)range;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    marked = s.length ? [s copy] : nil;
}

- (void)unmarkText
{
    NSString *s = marked;
    marked = nil;
    if (s.length)
        [ctl typeText:s];
}

- (NSRange)selectedRange
{
    return NSMakeRange(0, 0);
}

- (NSRange)markedRange
{
    return marked ? NSMakeRange(0, marked.length) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText
{
    return marked != nil;
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    (void)range;
    (void)actual;
    return nil;
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    /* The input method's window goes at the bottom left of the view,
     * where the filter and the Console line are. */
    (void)range;
    (void)actual;
    NSRect r = NSMakeRect(8, self.bounds.size.height - 24, 1, 16);
    return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)p
{
    (void)p;
    return NSNotFound;
}

@end

/* ---- Keys ---- */

/* The AU_KEY_* of a key without a character, or AU_KEY_NONE. */
static int key_of(NSEvent *e)
{
    NSString *c = e.charactersIgnoringModifiers;
    unichar ch = c.length ? [c characterAtIndex:0] : 0;
    switch (ch) {
    case NSLeftArrowFunctionKey: return AU_KEY_LEFT;
    case NSRightArrowFunctionKey: return AU_KEY_RIGHT;
    case NSUpArrowFunctionKey: return AU_KEY_UP;
    case NSDownArrowFunctionKey: return AU_KEY_DOWN;
    case NSPageUpFunctionKey: return AU_KEY_PAGE_UP;
    case NSPageDownFunctionKey: return AU_KEY_PAGE_DOWN;
    case NSHomeFunctionKey: return AU_KEY_HOME;
    case NSEndFunctionKey: return AU_KEY_END;
    case 0x7F: return AU_KEY_BACKSPACE; /* the Delete key */
    case NSDeleteFunctionKey: return AU_KEY_DELETE;
    case 0x1B: return AU_KEY_ESCAPE;
    case '\t':
    case 0x19: return AU_KEY_TAB; /* Tab, Shift-Tab */
    case '\r':
    case 0x03: return AU_KEY_RETURN; /* Return, Enter */
    default: return AU_KEY_NONE;
    }
}

@implementation AuDevWinCtl

/* ---- The controls from the model ---- */

static void sync_scroller(NSScroller *s, const AuTextView *tv)
{
    if (!s)
        return;
    int page = tv->visible_rows > 0 ? tv->visible_rows : 1;
    uint32_t max = au_textview_max_top(tv);
    s.enabled = max > 0;
    s.knobProportion = max > 0 ? (CGFloat)page / (CGFloat)((double)max + page) : 1.0;
    s.doubleValue = max > 0 ? (double)tv->top / (double)max : 0;
}

- (void)syncScroll
{
    syncing = true;
    sync_scroller(vscroll, &m.tv);
    sync_scroller(dscroll, &m.dv);
    syncing = false;
}

- (void)syncToggles
{
    syncing = true;
    for (int i = 0; i < ntoggles; i++) {
        NSControlStateValue want = m.toggle_on[i] ? NSControlStateValueOn : NSControlStateValueOff;
        if (toggles[i].state != want)
            toggles[i].state = want;
    }
    syncing = false;
}

/* After the model changed: the scroll bars, the toggles and the pixels. */
- (void)update
{
    if (closed)
        return;
    [self syncScroll];
    [self syncToggles];
    view.needsDisplay = YES;
}

/* The controls at the model's rectangles for its size. A native control
 * that needs more room than the Mac OS 9 one gets it, and the ones after
 * it move along. */
- (void)place
{
    AuDevWinAreas a;
    au_devwin_areas(&m, &a);
    int W = a.all.right, sw = scroll_w();
    if (vscroll)
        vscroll.frame = NSMakeRect(W - sw, a.main.top, sw, a.main.bottom - a.main.top);
    if (dscroll) {
        dscroll.hidden = !a.has_detail;
        dscroll.frame = NSMakeRect(W - sw, a.detail.top, sw, a.detail.bottom - a.detail.top);
    }
    AuDevRect r;
    CGFloat right = 0;
    for (int i = 0; i < nbuttons; i++) {
        au_devwin_button_rect(&m, i, &r);
        NSSize fit = buttons[i].fittingSize;
        CGFloat w = fit.width > r.right - r.left ? fit.width : r.right - r.left;
        CGFloat x = r.left > right + 6 || i == 0 ? r.left : right + 6;
        CGFloat h = fit.height;
        buttons[i].frame = NSMakeRect(x, r.top + ((r.bottom - r.top) - h) / 2, w, h);
        right = x + w;
    }
    footer_right = (int)ceil(right);
    right = 0;
    for (int i = 0; i < ntoggles; i++) {
        au_devwin_toggle_rect(&m, i, &r);
        NSSize fit = toggles[i].fittingSize;
        CGFloat x = r.left > right + 8 || i == 0 ? r.left : right + 8;
        if (x + fit.width > W - 8)
            x = W - 8 - fit.width > 0 ? W - 8 - fit.width : 0;
        toggles[i].frame = NSMakeRect(x, r.top + ((r.bottom - r.top) - fit.height) / 2, fit.width, fit.height);
        right = x + fit.width;
    }
}

- (void)resized
{
    if (closed)
        return;
    NSSize s = view.bounds.size;
    int w = (int)s.width, h = (int)s.height;
    if (w <= 0 || h <= 0)
        return;
    if (w != m.width || h != m.height)
        au_devwin_resize(&m, w, h);
    [self place];
    [self syncScroll];
    view.needsDisplay = YES;
}

/* ---- Painting ---- */

/* Text in the monospace face, its end kept in view when it is wider than
 * room; returns the x after it. */
static float draw_tail(AuCanvas *c, AuDevWin *m, float x, float base, float room, const char *text, size_t len,
                       AuColor ink)
{
    AuTextView *tv = &m->tv;
    while (len && au_text_width_list(tv->font, tv->size, text, len) > room) {
        size_t adv = 1;
        au_utf8_decode(text, len, &adv);
        if (adv < 1 || adv > len)
            adv = 1;
        text += adv;
        len -= adv;
    }
    return x + au_draw_text_list(c, tv->font, tv->size, x, base, text, len, ink);
}

static void paint_model(AuDevWinCtl *dw, AuCanvas *c)
{
    AuDevWin *m = &dw->m;
    AuDevWinAreas a;
    au_devwin_areas(m, &a);
    bool active = dw->win.keyWindow;
    m->tv.active = m->dv.active = active;
    m->tv.highlight = m->dv.highlight = au_cocoa_wh.browser->highlight;
    int W = a.all.right, H = a.all.bottom;
    au_fill_rect(c, (AuRect){0, 0, (float)W, (float)H}, AU_RGB(255, 255, 255));
    AuFontFace *small = au_fontset_match(au_cocoa_wh.fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    if (a.has_band) {
        int th = a.band.bottom;
        au_fill_rect(c, (AuRect){0, 0, (float)W, (float)th}, AU_RGB(221, 221, 221));
        au_fill_rect(c, (AuRect){0, (float)(th - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (a.has_filter) {
            if (small)
                au_draw_text(c, small, 10, 8, 16, "Filter:", 7, AU_RGB(40, 40, 40));
            AuRect f = {(float)a.filter.left, (float)a.filter.top, (float)(a.filter.right - a.filter.left),
                        (float)(a.filter.bottom - a.filter.top)};
            bool focus = m->filter_focus && active;
            au_fill_rect(c, f, focus ? AU_RGB(0, 0, 0) : AU_RGB(119, 119, 119));
            au_fill_rect(c, (AuRect){f.x + 1, f.y + 1, f.w - 2, f.h - 2}, AU_RGB(255, 255, 255));
            if (m->tv.font) {
                float x = draw_tail(c, m, f.x + 4, f.y + 12, f.w - 10, au_buf_cstr(&m->filter), m->filter.len,
                                    AU_RGB(0, 0, 0));
                if (focus)
                    au_fill_rect(c, (AuRect){x + 1, f.y + 3, 1, f.h - 6}, AU_RGB(0, 0, 0));
            }
        } else if (small) {
            /* Report a Problem: what the text is, and what to do with it. */
            const AuDevWinKindInfo *info = au_devwin_kind(m->kind);
            char buf[200];
            if (info->band_text[0]) {
                au_chrome_ellipsize(small, 10, info->band_text[0], (float)(W - 16), buf, sizeof buf);
                au_draw_text(c, small, 10, 8, 14, buf, strlen(buf), AU_RGB(0, 0, 0));
            }
            if (info->band_text[1])
                au_draw_text(c, small, 10, 8, 30, info->band_text[1], strlen(info->band_text[1]), AU_RGB(40, 40, 40));
        }
    }
    if (a.has_head) {
        au_fill_rect(c, (AuRect){0, (float)a.head.top, (float)W, AU_DEVWIN_HEAD_H}, AU_RGB(238, 238, 238));
        au_fill_rect(c, (AuRect){0, (float)(a.head.bottom - 1), (float)W, 1}, AU_RGB(153, 153, 153));
        if (m->tv.font)
            au_draw_text_list(c, m->tv.font, m->tv.size, (float)m->tv.gutter, (float)(a.head.top + 11), m->header,
                              strlen(m->header), AU_RGB(0, 0, 0));
    }
    au_textview_paint(&m->tv, c, 0, a.main.top);
    if (a.has_detail) {
        au_fill_rect(c, (AuRect){0, (float)(a.detail.top - 1), (float)W, 1}, AU_RGB(102, 102, 102));
        au_textview_paint(&m->dv, c, 0, a.detail.top);
    }
    if (a.has_input) {
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, AU_DEVWIN_INPUT_H}, AU_RGB(255, 255, 255));
        au_fill_rect(c, (AuRect){0, (float)a.input.top, (float)W, 1}, AU_RGB(153, 153, 153));
        if (m->tv.font) {
            float base = (float)a.input.top + 13;
            float x = au_draw_text_list(c, m->tv.font, m->tv.size, 5, base, "> ", 2, AU_RGB(0, 51, 170));
            x = draw_tail(c, m, 5 + x, base, (float)(W - scroll_w() - 24), au_buf_cstr(&m->input), m->input.len,
                          AU_RGB(0, 0, 0));
            if (active && !m->filter_focus)
                au_fill_rect(c, (AuRect){x + 1, (float)a.input.top + 3, 1, AU_DEVWIN_INPUT_H - 6}, AU_RGB(0, 0, 0));
        }
    }
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, AU_DEVWIN_BAR_H}, AU_RGB(221, 221, 221));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H), (float)W, 1}, AU_RGB(102, 102, 102));
    au_fill_rect(c, (AuRect){0, (float)(H - AU_DEVWIN_BAR_H + 1), (float)W, 1}, AU_RGB(255, 255, 255));
    if (small && m->footer[0]) {
        int x = dw->nbuttons ? dw->footer_right + 10 : 10;
        char buf[320];
        au_chrome_ellipsize(small, 10, m->footer, (float)(W - x - 8), buf, sizeof buf);
        au_draw_text(c, small, 10, (float)x, (float)(H - AU_DEVWIN_BAR_H + 20), buf, strlen(buf), AU_RGB(40, 40, 40));
    }
}

- (void)paintInto:(AuDevView *)v
{
    if (closed)
        return;
    NSSize s = v.bounds.size;
    int w = (int)s.width, h = (int)s.height;
    if (w <= 0 || h <= 0)
        return;
    if (w != m.width || h != m.height)
        au_devwin_resize(&m, w, h);
    size_t stride = (size_t)w * 4;
    if (!v->pixels || v->pw != w || v->ph != h) {
        v->pixels = [NSMutableData dataWithLength:stride * (size_t)h];
        v->pw = w;
        v->ph = h;
    }
    if (!v->pixels)
        return;
    AuSurface *surf = au_surface_wrap(v->pixels.mutableBytes, w, h, (int)stride);
    if (!surf)
        return;
    AuCanvas *c = au_canvas_new(surf);
    if (c) {
        paint_model(self, c);
        au_canvas_free(c);
    }
    au_surface_free(surf);
    /* Every pixel is painted opaque. */
    NSData *d = [NSData dataWithData:v->pixels];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)d);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, stride, cs,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img)
        return;
    v.layer.contents = (__bridge id)img;
    CGImageRelease(img);
}

/* ---- Controls ---- */

- (void)buttonClicked:(NSButton *)b
{
    if (closed || au_cocoa_dlg_running())
        return;
    NSInteger i = b.tag;
    const AuDevWinButton *list;
    int n = au_devwin_buttons(m.kind, &list);
    if (i < 0 || i >= n)
        return;
    AuDevWinCtl *keep = self;
    AuDevWinKind kind = m.kind;
    AuDevWinResult r = au_devwin_run(&m, list[i].action);
    /* Find waits on a dialog and Run Timing shows a window: check that
     * this one is still open. */
    if (g_dev[kind] != keep || keep->closed)
        return;
    if (r == AU_DEVWIN_FAILED)
        NSBeep();
    [self update];
}

- (void)toggleClicked:(NSButton *)b
{
    if (syncing || closed)
        return;
    NSInteger i = b.tag;
    bool on = b.state == NSControlStateValueOn;
    const AuDevWinToggle *t;
    int n = au_devwin_toggles(m.kind, &t);
    if (i < 0 || i >= n || on == m.toggle_on[i] || (t[i].radio && !on)) {
        [self syncToggles];
        return;
    }
    au_devwin_toggle(&m, (int)i);
    [self update];
}

- (void)scrolled:(NSScroller *)s
{
    if (syncing || closed)
        return;
    AuDevWinPane pane = s == dscroll ? AU_DEVWIN_PANE_DETAIL : AU_DEVWIN_PANE_MAIN;
    AuTextView *tv = au_devwin_view(&m, pane);
    if (!tv)
        return;
    uint32_t max = au_textview_max_top(tv);
    switch (s.hitPart) {
    case NSScrollerKnob:
    case NSScrollerKnobSlot:
        au_devwin_scroll_to(&m, pane, (uint32_t)(s.doubleValue * max + 0.5));
        view.needsDisplay = YES; /* the knob stays where the pointer has it */
        return;
    case NSScrollerDecrementPage: au_devwin_scroll(&m, pane, AU_KEY_PAGE_UP); break;
    case NSScrollerIncrementPage: au_devwin_scroll(&m, pane, AU_KEY_PAGE_DOWN); break;
    default: return;
    }
    [self update];
}

/* ---- The mouse ---- */

- (void)stopDragTimer
{
    [drag_timer invalidate];
    drag_timer = nil;
}

/* Past a text pane's top or bottom the selection scrolls a row a call:
 * this paces the calls while the pointer stays there. */
- (void)dragTick:(NSTimer *)t
{
    (void)t;
    if (closed || !dragging) {
        [self stopDragTimer];
        return;
    }
    if (au_devwin_mouse_drag(&m, drag_pane, (int)mouse.x, (int)mouse.y))
        [self update];
}

- (void)mouseDown:(NSEvent *)e at:(NSPoint)p
{
    if (closed || au_cocoa_dlg_running())
        return;
    [win makeFirstResponder:view];
    NSInteger n = e.clickCount;
    int clicks = n < 1 ? 1 : n > 3 ? 3 : (int)n;
    mouse = p;
    AuDevWinPane pane = au_devwin_mouse_down(&m, (int)p.x, (int)p.y, clicks,
                                             (e.modifierFlags & NSEventModifierFlagShift) != 0);
    [self update];
    AuTextView *tv = au_devwin_view(&m, pane);
    dragging = tv && !tv->select_rows;
    drag_pane = pane;
}

- (void)mouseDragged:(NSEvent *)e at:(NSPoint)p
{
    (void)e;
    if (closed || !dragging)
        return;
    mouse = p;
    if (au_devwin_mouse_drag(&m, drag_pane, (int)p.x, (int)p.y))
        [self update];
    AuDevWinAreas a;
    au_devwin_areas(&m, &a);
    const AuDevRect *r = drag_pane == AU_DEVWIN_PANE_DETAIL ? &a.detail : &a.main;
    bool outside = p.y < r->top || p.y >= r->bottom;
    if (outside && !drag_timer) {
        __weak AuDevWinCtl *weak = self;
        drag_timer = [NSTimer timerWithTimeInterval:0.04
                                            repeats:YES
                                              block:^(NSTimer *t) {
                                                [weak dragTick:t];
                                              }];
        [[NSRunLoop currentRunLoop] addTimer:drag_timer forMode:NSRunLoopCommonModes];
    } else if (!outside && drag_timer)
        [self stopDragTimer];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    [self stopDragTimer];
    if (!closed && dragging)
        au_devwin_mouse_up(&m, drag_pane);
    dragging = false;
}

- (void)wheel:(NSEvent *)e at:(NSPoint)p
{
    if (closed || au_cocoa_dlg_running())
        return;
    AuDevWinPane pane = au_devwin_pane_at(&m, (int)p.x, (int)p.y);
    if (pane != AU_DEVWIN_PANE_DETAIL)
        pane = AU_DEVWIN_PANE_MAIN;
    AuTextView *tv = au_devwin_view(&m, pane);
    double dy = e.scrollingDeltaY;
    if (!tv || dy == 0)
        return;
    if (pane != view->wheel_pane) {
        view->wheel_pane = pane;
        view->wheel_acc = 0;
    }
    double rows = e.hasPreciseScrollingDeltas ? -dy / (tv->row_h > 0 ? tv->row_h : 12) : -dy * 3;
    view->wheel_acc += rows;
    long whole = (long)view->wheel_acc; /* toward zero */
    if (!whole)
        return;
    view->wheel_acc -= (double)whole;
    long top = (long)tv->top + whole;
    if (top < 0)
        top = 0;
    au_devwin_scroll_to(&m, pane, (uint32_t)top);
    [self update];
}

/* ---- Keys ---- */

- (bool)key:(NSEvent *)e
{
    if (closed || au_cocoa_dlg_running())
        return true;
    /* Command keys reached the menu bar first; Control keys are not ours. */
    if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))
        return false;
    if (key_of(e) != AU_KEY_NONE && !view->marked) {
        [self typeKey:e];
        return true;
    }
    /* Characters, and every key while text is being composed, go through
     * the text input system: insertText and doCommandBySelector come back. */
    view->key_event = e;
    [view interpretKeyEvents:@[ e ]];
    view->key_event = nil;
    return true;
}

- (void)apply:(AuDevWinResult)r
{
    if (r == AU_DEVWIN_NOTHING)
        return;
    if (r == AU_DEVWIN_FAILED)
        NSBeep();
    [self update];
}

/* A key without a character (an arrow, Return, Delete). */
- (void)typeKey:(NSEvent *)e
{
    int key = key_of(e);
    if (closed || key == AU_KEY_NONE)
        return;
    [self apply:au_devwin_key(&m, key, NULL)];
}

/* Typed or composed text, a character at a time as the model takes it. */
- (void)typeText:(NSString *)s
{
    if (closed || au_cocoa_dlg_running())
        return;
    __block AuDevWinResult last = AU_DEVWIN_NOTHING;
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *one, NSRange r1, NSRange r2, BOOL *stop) {
                         (void)r1;
                         (void)r2;
                         (void)stop;
                         unichar ch = one.length ? [one characterAtIndex:0] : 0;
                         if (!ch || ch < 0x20 || ch == 0x7F || (ch >= 0xF700 && ch <= 0xF8FF))
                             return; /* not a character */
                         const char *text = one.UTF8String;
                         if (!text || closed)
                             return;
                         AuDevWinResult r = au_devwin_key(&m, AU_KEY_NONE, text);
                         if (r != AU_DEVWIN_NOTHING)
                             last = r;
                       }];
    [self apply:last];
}

/* ---- The window ---- */

- (void)windowDidBecomeKey:(NSNotification *)n
{
    (void)n;
    view.needsDisplay = YES;
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    if (view->marked) {
        /* Text half composed is dropped, as a text field drops it. */
        [view.inputContext discardMarkedText];
        view->marked = nil;
    }
    view.needsDisplay = YES;
}

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (!closed && !au_cocoa_dlg_running() && au_devwin_refresh(&m, false))
        [self update];
}

/* The window's place into the develop.windows preference. */
- (void)savePlace
{
    AuBrowser *b = au_cocoa_wh.browser;
    if (!b || m.width <= 0 || m.height <= 0)
        return;
    NSRect cr = [win contentRectForFrameRect:win.frame];
    NSScreen *main = NSScreen.screens.firstObject;
    CGFloat top = main ? NSMaxY(main.frame) : NSMaxY(cr);
    int place[4] = {(int)lround(NSMinX(cr)), (int)lround(top - NSMaxY(cr)), m.width, m.height};
    AuBuf out;
    au_buf_init(&out);
    au_dev_window_set(b->prefs.dev_windows, au_devwin_kind(m.kind)->name, place, &out);
    if (strcmp(au_buf_cstr(&out), b->prefs.dev_windows ? b->prefs.dev_windows : "") != 0) {
        au_prefs_set_str(&b->prefs.dev_windows, au_buf_cstr(&out));
        b->prefs_gen++; /* the caller saves the preferences file */
    }
    au_buf_free(&out);
}

- (BOOL)windowShouldClose:(NSWindow *)w
{
    (void)w;
    return !au_cocoa_dlg_running(); /* a dialog of this window is open */
}

- (void)windowWillClose:(NSNotification *)n
{
    (void)n;
    if (closed)
        return;
    AuDevWinCtl *keep = self; /* g_dev held the last reference */
    [self savePlace];
    closed = true;
    [timer invalidate];
    timer = nil;
    [self stopDragTimer];
    win.delegate = nil;
    if (g_dev[m.kind] == keep)
        g_dev[m.kind] = nil;
    au_devwin_free(&m);
    au_cocoa_dlg_windows_changed();
    (void)keep;
}

@end

/* ---- What the model asks of the system ---- */

static AuPage *h_page(void *ctx)
{
    (void)ctx;
    return front_page();
}

static AuBrowserWindow *h_window(void *ctx)
{
    (void)ctx;
    return au_cocoa_wh.front_window ? au_cocoa_wh.front_window(au_cocoa_wh.ctx) : NULL;
}

static uint32_t h_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(uint64_t)(NSProcessInfo.processInfo.systemUptime * 1000.0);
}

static void h_host_info(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn)
{
    (void)ctx;
    if (au_cocoa_wh.host_info)
        au_cocoa_wh.host_info(au_cocoa_wh.ctx, sys, h, conn);
}

static void h_connections(void *ctx, AuBuf *out)
{
    (void)ctx;
    if (au_cocoa_wh.connections)
        au_cocoa_wh.connections(au_cocoa_wh.ctx, out);
}

static void h_status_sections(void *ctx, AuBuf *out)
{
    (void)ctx;
    if (au_cocoa_wh.status_sections)
        au_cocoa_wh.status_sections(au_cocoa_wh.ctx, out);
    else if (au_cocoa_wh.aura_app)
        au_app_net_status(au_cocoa_wh.aura_app, out);
}

static const AuTimingTest *h_timing(void *ctx, const AuPage *p)
{
    (void)ctx;
    return g_timing.ran && g_timing_page == (const void *)p ? &g_timing : NULL;
}

static const AuPaintBench *h_bench(void *ctx)
{
    (void)ctx;
    return &g_bench;
}

static bool h_copy(void *ctx, const char *text, size_t len)
{
    (void)ctx;
    return au_cocoa_dlg_copy(text, len);
}

static void h_save_text(void *ctx, const char *prompt, const char *name, const char *text, size_t len)
{
    AuDevWinCtl *dw = (__bridge AuDevWinCtl *)ctx;
    NSData *data = [NSData dataWithBytes:text length:len];
    NSSavePanel *p = [NSSavePanel savePanel];
    p.message = au_cocoa_dlg_str(prompt ? prompt : "Save");
    if (name)
        p.nameFieldStringValue = au_cocoa_dlg_str(name);
    /* The text is copied, so nothing waits on the panel: a sheet on the
     * window, writing when it is answered. */
    void (^done)(NSModalResponse) = ^(NSModalResponse r) {
      if (r != NSModalResponseOK || !p.URL)
          return;
      NSError *err = nil;
      if (![data writeToURL:p.URL options:NSDataWritingAtomic error:&err])
          au_cocoa_dlg_alert(dw->closed ? front_ns_window() : dw->win, "The report could not be saved.",
                             err.localizedDescription.UTF8String);
    };
    if (!dw->closed && dw->win.visible && !dw->win.attachedSheet) {
        [p beginSheetModalForWindow:dw->win completionHandler:done];
    } else {
        au_cocoa_dlg_begin();
        NSModalResponse r = [p runModal];
        au_cocoa_dlg_end();
        done(r);
    }
}

static void h_open_url(void *ctx, const char *url)
{
    (void)ctx;
    if (au_cocoa_wh.open_url)
        au_cocoa_wh.open_url(au_cocoa_wh.ctx, url, true);
}

static char *h_ask_text(void *ctx, const char *prompt, const char *initial)
{
    AuDevWinCtl *dw = (__bridge AuDevWinCtl *)ctx;
    return au_cocoa_dlg_ask_text(dw->win, "Find", prompt, initial);
}

static void h_run_timing(void *ctx)
{
    (void)ctx;
    au_cocoa_devwin_run_timing();
}

static void h_title_changed(void *ctx, const char *title)
{
    AuDevWinCtl *dw = (__bridge AuDevWinCtl *)ctx;
    if (dw->win && !dw->closed) {
        dw->win.title = au_cocoa_dlg_str(title);
        au_cocoa_dlg_windows_changed();
    }
}

static void h_record_headers(void *ctx)
{
    (void)ctx;
    if (au_cocoa_wh.record_headers)
        au_cocoa_wh.record_headers(au_cocoa_wh.ctx);
}

/* ---- Showing ---- */

static void overlay(AuDevWinCtl *dw, NSControl *c)
{
    c.refusesFirstResponder = YES; /* the painted view keeps the keyboard */
    [dw->view addSubview:c];
}

static NSScroller *new_scroller(AuDevWinCtl *dw)
{
    NSScroller *s = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, scroll_w(), 100)];
    s.scrollerStyle = NSScrollerStyleLegacy;
    s.target = dw;
    s.action = @selector(scrolled:);
    overlay(dw, s);
    return s;
}

/* Where a saved top-left (points from the main screen's top-left) puts
 * a content of size w by h, or false when no screen would show its title
 * bar. */
static bool saved_frame(NSWindow *win, const int place[4], int w, int h, NSRect *out)
{
    NSScreen *main = NSScreen.screens.firstObject;
    if (!main || (place[0] == 0 && place[1] == 0))
        return false;
    NSRect cr = NSMakeRect(place[0], NSMaxY(main.frame) - place[1] - h, w, h);
    NSRect fr = [win frameRectForContentRect:cr];
    NSRect title = NSMakeRect(NSMinX(fr), NSMaxY(cr), NSWidth(fr), NSMaxY(fr) - NSMaxY(cr));
    for (NSScreen *s in NSScreen.screens) {
        NSRect seen = NSIntersectionRect(s.visibleFrame, title);
        if (NSWidth(seen) >= 60 && NSHeight(seen) >= 1) {
            *out = fr;
            return true;
        }
    }
    return false;
}

void au_cocoa_devwin_show(AuDevWinKind kind)
{
    if ((int)kind < 0 || kind >= AU_DEVWIN_KINDS || !au_cocoa_wh.browser || !au_cocoa_wh.fonts)
        return;
    AuDevWinCtl *dw = g_dev[kind];
    if (dw) {
        if (au_devwin_refresh(&dw->m, true))
            [dw update];
        [dw->win makeKeyAndOrderFront:nil];
        return;
    }
    dw = [AuDevWinCtl new];
    AuDevWinHost host;
    memset(&host, 0, sizeof host);
    /* g_dev owns the window object; the model is freed before it goes. */
    host.ctx = (__bridge void *)dw;
    host.page = h_page;
    host.window = h_window;
    host.now_ms = h_now;
    host.host_info = h_host_info;
    host.connections = h_connections;
    host.status_sections = h_status_sections;
    host.timing = h_timing;
    host.bench = h_bench;
    host.copy = h_copy;
    host.save_text = h_save_text;
    host.open_url = h_open_url;
    host.ask_text = h_ask_text;
    host.run_timing = h_run_timing;
    /* busy stays NULL: AppKit has no public busy cursor, and a Console
     * line runs for a moment. */
    host.title_changed = h_title_changed;
    host.record_headers = h_record_headers;
    au_devwin_init(&dw->m, kind, au_cocoa_wh.browser, au_cocoa_wh.fonts, scroll_w(), &host);
    const AuDevWinKindInfo *info = au_devwin_kind(kind);

    int saved[4], width = 720, height = info->default_h;
    bool have_saved = au_dev_window_get(au_cocoa_wh.browser->prefs.dev_windows, info->name, saved) &&
                      saved[2] >= 200 && saved[3] >= 160;
    if (have_saved) {
        width = saved[2];
        height = saved[3];
    }
    if (height < 160)
        height = 160;
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.releasedWhenClosed = NO;
    w.title = au_cocoa_dlg_str(au_devwin_title(&dw->m));
    w.contentMinSize = NSMakeSize(320, 160);
    w.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    w.delegate = dw;
    dw->win = w;

    AuDevView *v = [[AuDevView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    v->ctl = dw;
    dw->view = v;
    w.contentView = v;

    dw->vscroll = new_scroller(dw);
    if (info->detail)
        dw->dscroll = new_scroller(dw);
    const AuDevWinButton *b;
    dw->nbuttons = au_devwin_buttons(kind, &b);
    if (dw->nbuttons > AU_DEVWIN_MAX_BUTTONS)
        dw->nbuttons = AU_DEVWIN_MAX_BUTTONS;
    for (int i = 0; i < dw->nbuttons; i++) {
        NSString *label = au_cocoa_dlg_str(b[i].label);
        if ([label hasSuffix:@"..."])
            label = [[label substringToIndex:label.length - 3] stringByAppendingString:@"…"];
        NSButton *btn = [NSButton buttonWithTitle:label target:dw action:@selector(buttonClicked:)];
        btn.controlSize = NSControlSizeSmall;
        btn.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        btn.tag = i;
        overlay(dw, btn);
        dw->buttons[i] = btn;
    }
    const AuDevWinToggle *t;
    dw->ntoggles = au_devwin_toggles(kind, &t);
    if (dw->ntoggles > AU_DEVWIN_MAX_TOGGLES)
        dw->ntoggles = AU_DEVWIN_MAX_TOGGLES;
    for (int i = 0; i < dw->ntoggles; i++) {
        NSString *label = au_cocoa_dlg_str(t[i].label);
        /* Radio buttons in one superview with one action form a group. */
        NSButton *c = t[i].radio ? [NSButton radioButtonWithTitle:label target:dw action:@selector(toggleClicked:)]
                                 : [NSButton checkboxWithTitle:label target:dw action:@selector(toggleClicked:)];
        c.controlSize = NSControlSizeSmall;
        c.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        c.state = dw->m.toggle_on[i] ? NSControlStateValueOn : NSControlStateValueOff;
        c.tag = i;
        overlay(dw, c);
        dw->toggles[i] = c;
    }

    g_dev[kind] = dw;
    au_devwin_resize(&dw->m, width, height);
    [dw place];
    au_devwin_refresh(&dw->m, true);
    [dw syncScroll];
    __weak AuDevWinCtl *weak = dw;
    dw->timer = [NSTimer scheduledTimerWithTimeInterval:0.25
                                                repeats:YES
                                                  block:^(NSTimer *tm) {
                                                    [weak tick:tm];
                                                  }];
    NSRect fr;
    if (have_saved && saved_frame(w, saved, width, height, &fr))
        [w setFrame:fr display:NO];
    else
        [w center];
    v.needsDisplay = YES;
    [w makeKeyAndOrderFront:nil];
    [w makeFirstResponder:v];
    au_cocoa_dlg_windows_changed();
}

void au_cocoa_devwin_run_timing(void)
{
    AuPage *p = front_page();
    if (!p || !au_cocoa_wh.timing_test || !au_cocoa_wh.timing_test(au_cocoa_wh.ctx, &g_timing)) {
        NSBeep();
        return;
    }
    g_timing_page = p;
    au_cocoa_devwin_show(AU_DEVWIN_PERF);
}

void au_cocoa_devwin_run_benchmark(void)
{
    if (!au_cocoa_wh.benchmark || !au_cocoa_wh.benchmark(au_cocoa_wh.ctx, &g_bench)) {
        NSBeep();
        return;
    }
    au_cocoa_devwin_show(AU_DEVWIN_STATUS);
}

static AuDevWinCtl *dev_of(NSWindow *w)
{
    if (!w)
        return nil;
    for (int i = 0; i < AU_DEVWIN_KINDS; i++)
        if (g_dev[i] && !g_dev[i]->closed && g_dev[i]->win == w)
            return g_dev[i];
    return nil;
}

bool au_cocoa_devwin_owns(NSWindow *w) { return dev_of(w) != nil; }

bool au_cocoa_devwin_edit(NSWindow *w, AuEditAction action)
{
    AuDevWinCtl *dw = dev_of(w);
    if (!dw || au_cocoa_dlg_running())
        return false;
    switch (action) {
    case AU_EDIT_COPY: {
        AuBuf b;
        au_buf_init(&b);
        bool ok = au_devwin_copy_text(&dw->m, &b);
        if (ok)
            ok = au_cocoa_dlg_copy(au_buf_cstr(&b), b.len);
        au_buf_free(&b);
        return ok;
    }
    case AU_EDIT_PASTE: {
        if (!au_devwin_kind(dw->m.kind)->filter)
            return false;
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char *text = s.UTF8String;
        if (text && au_devwin_paste(&dw->m, text))
            [dw update];
        else
            NSBeep();
        return true;
    }
    case AU_EDIT_SELECT_ALL:
        if (!au_devwin_select_all(&dw->m))
            return false;
        [dw update];
        return true;
    default: return false;
    }
}

void au_cocoa_devwin_find(NSWindow *w)
{
    AuDevWinCtl *dw = dev_of(w);
    if (!dw || au_cocoa_dlg_running())
        return;
    AuDevWinKind kind = dw->m.kind;
    AuDevWinResult r = au_devwin_ask_find(&dw->m);
    if (g_dev[kind] != dw || dw->closed)
        return;
    if (r == AU_DEVWIN_FAILED)
        NSBeep();
    [dw update];
}

void au_cocoa_devwin_find_again(NSWindow *w, bool forward)
{
    AuDevWinCtl *dw = dev_of(w);
    if (!dw || au_cocoa_dlg_running())
        return;
    if (!au_devwin_find_next(&dw->m, forward))
        NSBeep();
    [dw update];
}

bool au_cocoa_devwin_can_find_again(NSWindow *w)
{
    AuDevWinCtl *dw = dev_of(w);
    return dw && au_devwin_can_find_again(&dw->m);
}

void au_cocoa_devwin_shutdown(void)
{
    for (int i = 0; i < AU_DEVWIN_KINDS; i++)
        if (g_dev[i])
            [g_dev[i]->win close];
}

bool au_cocoa_devwin_built(void) { return true; }
