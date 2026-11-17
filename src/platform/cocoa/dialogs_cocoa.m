/* Helpers shared by the secondary windows: the modal dialogs, the details
 * window, the pasteboard (dialogs_cocoa.h). */
#import "dialogs_cocoa.h"

#include <string.h>
#include <time.h>

static int g_dialogs; /* modal dialogs running */

NSString *au_cocoa_dlg_str(const char *utf8)
{
    NSString *s = utf8 ? [NSString stringWithUTF8String:utf8] : nil;
    if (!s && utf8)
        s = [[NSString alloc] initWithBytes:utf8 length:strlen(utf8) encoding:NSISOLatin1StringEncoding];
    return s ? s : @"";
}

bool au_cocoa_dlg_running(void) { return g_dialogs > 0; }

void au_cocoa_dlg_begin(void)
{
    g_dialogs++;
    if (au_cocoa_wh.modal_begin)
        au_cocoa_wh.modal_begin(au_cocoa_wh.ctx);
}

void au_cocoa_dlg_end(void)
{
    if (au_cocoa_wh.modal_end)
        au_cocoa_wh.modal_end(au_cocoa_wh.ctx);
    g_dialogs--;
}

AuTime au_cocoa_dlg_now(void) { return (AuTime)time(NULL); }

void au_cocoa_dlg_windows_changed(void)
{
    if (au_cocoa_wh.windows_changed)
        au_cocoa_wh.windows_changed(au_cocoa_wh.ctx);
}

bool au_cocoa_dlg_copy(const char *text, size_t len)
{
    NSString *s = [[NSString alloc] initWithBytes:text length:len encoding:NSUTF8StringEncoding];
    if (!s)
        s = [[NSString alloc] initWithBytes:text length:len encoding:NSISOLatin1StringEncoding];
    if (!s)
        return false;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    return [pb setString:s forType:NSPasteboardTypeString];
}

/* ---- Modal dialogs ---- */

static NSAlert *new_alert(const char *title, const char *prompt)
{
    /* An alert has no title of its own: the prompt is its message, or the
     * title when there is no prompt. */
    NSAlert *a = [NSAlert new];
    a.messageText = au_cocoa_dlg_str(prompt && *prompt ? prompt : title ? title : "Aura");
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"]; /* AppKit gives a button named Cancel the Escape key */
    return a;
}

char *au_cocoa_dlg_ask_text(NSWindow *parent, const char *title, const char *prompt, const char *initial)
{
    (void)parent;
    NSAlert *a = new_alert(title, prompt);
    NSTextField *field = [NSTextField textFieldWithString:au_cocoa_dlg_str(initial)];
    field.frame = NSMakeRect(0, 0, 300, 22);
    field.usesSingleLineMode = YES;
    field.cell.scrollable = YES;
    a.accessoryView = field;
    [a layout];
    a.window.initialFirstResponder = field;
    au_cocoa_dlg_begin();
    NSModalResponse r = [a runModal];
    au_cocoa_dlg_end();
    if (r != NSAlertFirstButtonReturn)
        return NULL;
    const char *t = field.stringValue.UTF8String;
    if (!t)
        t = "";
    return au_strndup(t, strlen(t));
}

/* Radio buttons in one superview with one action form a group. */
@interface AuDlgRadioTarget : NSObject
- (void)picked:(id)sender;
@end

@implementation AuDlgRadioTarget
- (void)picked:(id)sender
{
    (void)sender;
}
@end

int au_cocoa_dlg_choose(NSWindow *parent, const char *title, const char *prompt, const char *const *items, int n,
                        int current)
{
    (void)parent;
    if (n <= 0)
        return -1;
    NSAlert *a = new_alert(title, prompt);
    AuDlgRadioTarget *target = [AuDlgRadioTarget new];
    NSMutableArray<NSButton *> *radios = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
    NSStackView *stack = [NSStackView new];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    for (int i = 0; i < n; i++) {
        NSButton *b = [NSButton radioButtonWithTitle:au_cocoa_dlg_str(items[i])
                                              target:target
                                              action:@selector(picked:)];
        b.state = i == current ? NSControlStateValueOn : NSControlStateValueOff;
        [stack addArrangedSubview:b];
        [radios addObject:b];
    }
    NSSize fit = stack.fittingSize;
    stack.frame = NSMakeRect(0, 0, fit.width > 200 ? fit.width : 200, fit.height);
    a.accessoryView = stack;
    au_cocoa_dlg_begin();
    NSModalResponse r = [a runModal];
    au_cocoa_dlg_end();
    if (r != NSAlertFirstButtonReturn)
        return -1;
    for (int i = 0; i < n; i++)
        if (radios[(NSUInteger)i].state == NSControlStateValueOn)
            return i;
    return -1;
}

void au_cocoa_dlg_alert(NSWindow *parent, const char *message, const char *detail)
{
    NSAlert *a = [NSAlert new];
    a.messageText = au_cocoa_dlg_str(message);
    if (detail)
        a.informativeText = au_cocoa_dlg_str(detail);
    if (parent && parent.visible && !parent.attachedSheet)
        [a beginSheetModalForWindow:parent completionHandler:nil];
    else {
        au_cocoa_dlg_begin();
        [a runModal];
        au_cocoa_dlg_end();
    }
}

/* ---- The details window ---- */

static NSMutableArray<NSWindow *> *g_text_windows; /* open details windows, kept alive until closed */

@interface AuDlgTextCloser : NSObject
@property(weak) NSWindow *window;
- (void)close:(id)sender;
@end

@implementation AuDlgTextCloser
- (void)close:(id)sender
{
    (void)sender;
    [self.window performClose:nil];
}
@end

static NSTextField *text_label(NSString *s, bool key)
{
    NSTextField *t = key ? [NSTextField labelWithString:s] : [NSTextField wrappingLabelWithString:s];
    if (key) {
        t.textColor = NSColor.secondaryLabelColor;
        t.alignment = NSTextAlignmentRight;
    } else {
        t.selectable = YES;
        t.preferredMaxLayoutWidth = 320;
    }
    return t;
}

void au_cocoa_dlg_show_text(NSWindow *parent, const char *title, const char *text)
{
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 300)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.releasedWhenClosed = NO;
    w.title = au_cocoa_dlg_str(title ? title : "Details");
    w.minSize = NSMakeSize(300, 160);

    /* "Label: value" lines in two columns; other lines span both. */
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
    grid.columnSpacing = 12;
    grid.rowSpacing = 4;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    const char *p = text ? text : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        NSString *line = [[NSString alloc] initWithBytes:p length:len encoding:NSUTF8StringEncoding];
        if (!line)
            line = [[NSString alloc] initWithBytes:p length:len encoding:NSISOLatin1StringEncoding];
        NSRange colon = [line rangeOfString:@": "];
        if (colon.location != NSNotFound) {
            NSString *k = [line substringToIndex:colon.location];
            NSString *v = [line substringFromIndex:colon.location + 2];
            [grid addRowWithViews:@[ text_label(k, true), text_label(v, false) ]];
        } else {
            NSGridRow *row = [grid addRowWithViews:@[ text_label(line, false), [NSGridCell emptyContentView] ]];
            [row mergeCellsInRange:NSMakeRange(0, 2)];
        }
        p += len;
        if (*p == '\n')
            p++;
    }
    if (grid.numberOfColumns >= 1)
        [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSScrollView *sv = [NSScrollView new];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    NSClipView *clip = [NSClipView new];
    clip.drawsBackground = NO;
    sv.contentView = clip;
    /* A flipped document view keeps the grid at the top. */
    NSView *doc = [NSView new];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:grid];
    sv.documentView = doc;

    AuDlgTextCloser *closer = [AuDlgTextCloser new];
    closer.window = w;
    NSButton *close = [NSButton buttonWithTitle:@"Close" target:closer action:@selector(close:)];
    close.keyEquivalent = @"\r";
    close.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *content = w.contentView;
    [content addSubview:sv];
    [content addSubview:close];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
        [sv.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
        [sv.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [close.topAnchor constraintEqualToAnchor:sv.bottomAnchor constant:8],
        [close.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [close.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
        [doc.topAnchor constraintEqualToAnchor:clip.topAnchor],
        [doc.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [doc.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
        [grid.topAnchor constraintEqualToAnchor:doc.topAnchor],
        [grid.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [grid.trailingAnchor constraintLessThanOrEqualToAnchor:doc.trailingAnchor],
        [grid.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor],
    ]];

    if (!g_text_windows)
        g_text_windows = [NSMutableArray new];
    [g_text_windows addObject:w];
    /* The closer lives as long as the window: the observer block holds it. */
    __block id observer = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowWillCloseNotification
                    object:w
                     queue:nil
                usingBlock:^(NSNotification *note) {
                  (void)closer;
                  NSWindow *closing = note.object;
                  [[NSNotificationCenter defaultCenter] removeObserver:observer];
                  observer = nil;
                  /* Released after the close finishes. */
                  dispatch_async(dispatch_get_main_queue(), ^{
                    [g_text_windows removeObject:closing];
                  });
                }];
    if (parent) {
        NSRect pf = parent.frame;
        [w setFrameTopLeftPoint:NSMakePoint(NSMinX(pf) + 40, NSMaxY(pf) - 40)];
    } else
        [w center];
    [w makeKeyAndOrderFront:nil];
}
