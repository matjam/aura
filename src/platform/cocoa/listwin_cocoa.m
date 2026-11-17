/* The Bookmarks, History, Downloads and Cookies windows.
 *
 * The rows, the selection, the buttons, what each action does and when
 * the list is stale are the model in src/browser/lists.c. This file holds
 * only what is about AppKit:
 *   - a view-based NSTableView in an NSScrollView with the columns the
 *     model names, reloaded when the model's generation moves; History's
 *     day headings are group rows;
 *   - the first column's indent, disclosure button and icon; a progress
 *     bar in the Downloads window's Progress column;
 *   - the model's buttons in a row under the list;
 *   - a 250 ms timer that refreshes a stale model;
 *   - Delete and Backspace run Delete, Return and a double click activate
 *     the row;
 *   - the AuListHost callbacks: the host's open_url and reveal_download,
 *     and the modal dialogs of dialogs_cocoa.m. */
#import "dialogs_cocoa.h"

#include <string.h>

@class AuListWin;

/* ---- The table: keys go to the window's model ---- */

@interface AuListTable : NSTableView
@property(weak) AuListWin *owner;
@end

/* ---- The first column's cell: [indent][disclosure][icon] text ---- */

@interface AuListFirstCell : NSTableCellView
@property(strong) NSButton *disclosure;
@property int depth;
@property bool folder;
@end

@implementation AuListFirstCell

- (void)layout
{
    [super layout];
    NSRect b = self.bounds;
    CGFloat x = 2 + self.depth * 16;
    CGFloat mid = NSMidY(b);
    self.disclosure.hidden = !self.folder;
    if (self.folder) {
        self.disclosure.frame = NSMakeRect(x, mid - 7, 14, 14);
        x += 16;
    }
    if (self.imageView.image) {
        self.imageView.hidden = NO;
        self.imageView.frame = NSMakeRect(x, mid - 8, 16, 16);
        x += 20;
    } else
        self.imageView.hidden = YES;
    NSSize ts = self.textField.fittingSize;
    self.textField.frame = NSMakeRect(x, mid - ts.height / 2, NSMaxX(b) - x - 2, ts.height);
}

@end

/* ---- Downloads' Progress column: a bar while the size is known ---- */

@interface AuListProgressCell : NSTableCellView
@property(strong) NSProgressIndicator *bar;
@end

@implementation AuListProgressCell

- (void)layout
{
    [super layout];
    NSRect b = self.bounds;
    CGFloat mid = NSMidY(b), x = 2;
    if (!self.bar.hidden) {
        self.bar.frame = NSMakeRect(x, mid - 6, 60, 12);
        x += 66;
    }
    NSSize ts = self.textField.fittingSize;
    self.textField.frame = NSMakeRect(x, mid - ts.height / 2, NSMaxX(b) - x - 2, ts.height);
}

@end

/* ---- A window ---- */

@interface AuListWin : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate> {
  @public
    AuListModel m;
    NSWindow *win;
    AuListTable *table;
    NSButton *buttons[AU_LIST_ACTIONS];
    AuListAction actions[AU_LIST_ACTIONS];
    int nbuttons;
    uint32_t shown_gen; /* the generation the table shows */
    bool shown;         /* the table was filled once */
    bool syncing;       /* the selection is being set from the model */
    bool closed;        /* the model is freed */
    NSTimer *timer;
}
- (void)activateRow:(NSInteger)row;
- (void)runAction:(AuListAction)a;
@end

static AuListWin *g_lists[AU_LIST_KINDS];

@implementation AuListTable

- (void)keyDown:(NSEvent *)e
{
    AuListWin *lw = self.owner;
    NSEventModifierFlags f = e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                                NSEventModifierFlagOption);
    if (lw && !f) {
        switch (e.keyCode) {
        case 51:  /* Delete (Backspace) */
        case 117: /* Forward Delete */
            if (au_list_can(&lw->m, AU_LIST_DELETE))
                [lw runAction:AU_LIST_DELETE];
            else
                NSBeep();
            return;
        case 36: /* Return */
        case 76: /* Enter */
            if (self.selectedRow >= 0)
                [lw activateRow:self.selectedRow];
            return;
        default: break;
        }
    }
    [super keyDown:e];
}

@end

static NSImage *icon_image(AuListIcon icon)
{
    const char *name = NULL;
    switch (icon) {
    case AU_LIST_ICON_FOLDER: name = "folder"; break;
    case AU_LIST_ICON_PAGE: name = "globe"; break;
    case AU_LIST_ICON_FILE: name = "doc"; break;
    case AU_LIST_ICON_COOKIE: name = "lock.shield"; break;
    case AU_LIST_ICON_STORAGE: name = "internaldrive"; break;
    default: return nil;
    }
    if (@available(macOS 11.0, *))
        return [NSImage imageWithSystemSymbolName:au_cocoa_dlg_str(name) accessibilityDescription:nil];
    return icon == AU_LIST_ICON_FOLDER ? [NSImage imageNamed:NSImageNameFolder] : nil;
}

static NSTextField *cell_label(void)
{
    NSTextField *t = [NSTextField labelWithString:@""];
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    t.cell.truncatesLastVisibleLine = YES;
    return t;
}

@implementation AuListWin

/* ---- Keeping the view in step with the model ---- */

- (void)enableButtons
{
    for (int i = 0; i < nbuttons; i++)
        buttons[i].enabled = au_list_can(&m, actions[i]);
}

- (void)syncView
{
    if (closed)
        return;
    syncing = true;
    if (!shown || shown_gen != m.generation) {
        [table reloadData];
        shown_gen = m.generation;
        shown = true;
    }
    NSInteger want = m.selected >= 0 && m.selected < m.n ? m.selected : -1;
    if (table.selectedRow != want) {
        if (want >= 0) {
            [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)want] byExtendingSelection:NO];
            [table scrollRowToVisible:want];
        } else
            [table deselectAll:nil];
    }
    syncing = false;
    [self enableButtons];
}

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (closed || au_cocoa_dlg_running())
        return;
    if (au_list_stale(&m)) {
        au_list_refresh(&m, au_cocoa_dlg_now());
        [self syncView];
    }
}

- (void)runAction:(AuListAction)a
{
    if (closed || au_cocoa_dlg_running())
        return;
    AuListWin *keep = self; /* a dialog's run loop may close windows */
    if (au_list_run(&m, a, au_cocoa_dlg_now()) == AU_LIST_FAILED)
        NSBeep();
    if (g_lists[m.kind] == keep)
        [keep syncView];
}

- (void)activateRow:(NSInteger)row
{
    if (closed || au_cocoa_dlg_running() || row < 0 || row >= m.n)
        return;
    AuListWin *keep = self;
    if (au_list_activate(&m, (int)row, au_cocoa_dlg_now()) == AU_LIST_FAILED)
        NSBeep();
    if (g_lists[m.kind] == keep)
        [keep syncView];
}

/* ---- Controls ---- */

- (void)buttonClicked:(NSButton *)b
{
    NSInteger i = b.tag;
    if (i >= 0 && i < nbuttons)
        [self runAction:actions[i]];
}

- (void)doubleClicked:(id)sender
{
    (void)sender;
    if (table.clickedRow >= 0)
        [self activateRow:table.clickedRow];
}

- (void)disclosureClicked:(NSButton *)b
{
    NSInteger row = [table rowForView:b];
    if (closed || row < 0 || row >= m.n || au_cocoa_dlg_running())
        return;
    au_list_toggle(&m, (int)row, au_cocoa_dlg_now());
    [self syncView];
}

/* ---- The table's data ---- */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return closed ? 0 : m.n;
}

- (BOOL)tableView:(NSTableView *)tv isGroupRow:(NSInteger)row
{
    (void)tv;
    return !closed && row >= 0 && row < m.n && m.items[row].type == AU_LIST_ITEM_GROUP;
}

- (BOOL)tableView:(NSTableView *)tv shouldSelectRow:(NSInteger)row
{
    (void)tv;
    /* A day heading is never selected (au_list_select). */
    return !closed && row >= 0 && row < m.n && m.items[row].type != AU_LIST_ITEM_GROUP;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    if (closed || row < 0 || row >= m.n)
        return nil;
    const AuListItem *it = &m.items[row];
    if (!column) {
        /* A group row: the heading across the columns. */
        NSTableCellView *c = [tv makeViewWithIdentifier:@"group" owner:self];
        if (!c) {
            c = [NSTableCellView new];
            c.identifier = @"group";
            NSTextField *t = cell_label();
            t.font = [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize];
            t.frame = NSMakeRect(4, 0, 400, 17);
            t.autoresizingMask = NSViewWidthSizable;
            [c addSubview:t];
            c.textField = t;
        }
        c.textField.stringValue = au_cocoa_dlg_str(it->cols[0]);
        return c;
    }
    int col = (int)column.identifier.integerValue;
    if (col < 0 || col >= AU_LIST_MAX_COLS)
        return nil;
    if (col == 0) {
        AuListFirstCell *c = [tv makeViewWithIdentifier:@"first" owner:self];
        if (!c) {
            c = [AuListFirstCell new];
            c.identifier = @"first";
            NSButton *d = [NSButton new];
            d.bezelStyle = NSBezelStyleDisclosure;
            [d setButtonType:NSButtonTypePushOnPushOff];
            d.title = @"";
            d.refusesFirstResponder = YES;
            d.target = self;
            d.action = @selector(disclosureClicked:);
            NSImageView *iv = [NSImageView new];
            iv.imageScaling = NSImageScaleProportionallyDown;
            NSTextField *t = cell_label();
            [c addSubview:d];
            [c addSubview:iv];
            [c addSubview:t];
            c.disclosure = d;
            c.imageView = iv;
            c.textField = t;
        }
        c.depth = it->depth;
        c.folder = it->type == AU_LIST_ITEM_FOLDER;
        c.disclosure.state = it->expanded ? NSControlStateValueOn : NSControlStateValueOff;
        c.imageView.image = icon_image(it->icon);
        c.textField.stringValue = au_cocoa_dlg_str(it->cols[0]);
        c.toolTip = it->summary && it->summary[0] ? au_cocoa_dlg_str(it->summary) : nil;
        c.needsLayout = YES;
        return c;
    }
    if (m.kind == AU_LIST_DOWNLOADS && col == 3) {
        AuListProgressCell *c = [tv makeViewWithIdentifier:@"progress" owner:self];
        if (!c) {
            c = [AuListProgressCell new];
            c.identifier = @"progress";
            NSProgressIndicator *bar = [NSProgressIndicator new];
            bar.style = NSProgressIndicatorStyleBar;
            bar.indeterminate = NO;
            bar.minValue = 0;
            bar.maxValue = 1000;
            bar.controlSize = NSControlSizeSmall;
            NSTextField *t = cell_label();
            [c addSubview:bar];
            [c addSubview:t];
            c.bar = bar;
            c.textField = t;
        }
        bool show = it->progress >= 0 &&
                    (it->state == AU_DL_ACTIVE || it->state == AU_DL_PAUSED || it->state == AU_DL_QUEUED);
        c.bar.hidden = !show;
        if (show)
            c.bar.doubleValue = it->progress;
        c.textField.stringValue = au_cocoa_dlg_str(it->cols[col]);
        c.needsLayout = YES;
        return c;
    }
    NSTableCellView *c = [tv makeViewWithIdentifier:@"text" owner:self];
    if (!c) {
        c = [NSTableCellView new];
        c.identifier = @"text";
        NSTextField *t = cell_label();
        t.translatesAutoresizingMaskIntoConstraints = NO;
        [c addSubview:t];
        c.textField = t;
        [NSLayoutConstraint activateConstraints:@[
            [t.leadingAnchor constraintEqualToAnchor:c.leadingAnchor constant:2],
            [t.trailingAnchor constraintEqualToAnchor:c.trailingAnchor constant:-2],
            [t.centerYAnchor constraintEqualToAnchor:c.centerYAnchor],
        ]];
    }
    NSString *s = au_cocoa_dlg_str(it->cols[col]);
    c.textField.stringValue = s;
    c.toolTip = s.length ? s : nil;
    return c;
}

- (void)tableViewSelectionDidChange:(NSNotification *)n
{
    (void)n;
    if (syncing || closed)
        return;
    au_list_select(&m, (int)table.selectedRow);
    [self syncView];
}

/* ---- The window ---- */

- (BOOL)windowShouldClose:(NSWindow *)w
{
    (void)w;
    return !au_cocoa_dlg_running(); /* the model is waiting on the dialog */
}

- (void)windowWillClose:(NSNotification *)n
{
    (void)n;
    if (closed)
        return;
    AuListWin *keep = self; /* g_lists held the last reference */
    closed = true;
    [timer invalidate];
    timer = nil;
    table.dataSource = nil;
    table.delegate = nil;
    win.delegate = nil;
    if (g_lists[m.kind] == keep)
        g_lists[m.kind] = nil;
    au_list_free(&m);
    au_cocoa_dlg_windows_changed();
    (void)keep;
}

@end

/* ---- The model's callbacks ---- */

static void h_open_url(void *ctx, const char *url, bool new_tab)
{
    (void)ctx;
    if (au_cocoa_wh.open_url)
        au_cocoa_wh.open_url(au_cocoa_wh.ctx, url, new_tab);
}

static char *h_ask_text(void *ctx, const char *prompt, const char *initial)
{
    AuListWin *lw = (__bridge AuListWin *)ctx;
    return au_cocoa_dlg_ask_text(lw->win, au_list_title(lw->m.kind), prompt, initial);
}

static void h_show_text(void *ctx, const char *title, const char *text)
{
    AuListWin *lw = (__bridge AuListWin *)ctx;
    au_cocoa_dlg_show_text(lw->win, title, text);
}

static int h_choose(void *ctx, AuListAction action, const char *const *items, int n, int current)
{
    AuListWin *lw = (__bridge AuListWin *)ctx;
    const char *title = au_list_title(lw->m.kind);
    const AuListButton *b;
    int nb = au_list_buttons(lw->m.kind, &b);
    for (int i = 0; i < nb; i++)
        if (b[i].action == action)
            title = b[i].label;
    return au_cocoa_dlg_choose(lw->win, title, NULL, items, n, current);
}

static bool h_reveal(void *ctx, const AuDownload *d)
{
    (void)ctx;
    return au_cocoa_wh.reveal_download && au_cocoa_wh.reveal_download(au_cocoa_wh.ctx, d);
}

static void h_rules_changed(void *ctx)
{
    (void)ctx;
    if (au_cocoa_wh.aura_app)
        au_app_cookie_rules_changed(au_cocoa_wh.aura_app);
}

/* ---- Entry points ---- */

enum { BAR_H = 44 }; /* the button row under the list */

void au_cocoa_list_show(AuListKind kind)
{
    if ((int)kind < 0 || kind >= AU_LIST_KINDS || !au_cocoa_wh.browser)
        return;
    if (g_lists[kind]) {
        [g_lists[kind]->win makeKeyAndOrderFront:nil];
        return;
    }
    AuListWin *lw = [AuListWin new];
    AuListSources src;
    AuApp *app = au_cocoa_wh.aura_app;
    au_list_sources(&src, au_cocoa_wh.browser, app ? au_app_cookie_jar(app) : NULL, app ? au_app_storage(app) : NULL);
    /* The model's ctx does not own the window object: g_lists does, and
     * the model is freed before it goes. */
    AuListHost host = {(__bridge void *)lw, h_open_url, h_ask_text, h_show_text, h_choose, h_reveal, h_rules_changed};
    au_list_init(&lw->m, kind, &src, &host, au_cocoa_dlg_now());

    CGFloat W = kind == AU_LIST_DOWNLOADS || kind == AU_LIST_COOKIES ? 720 : 640, H = 480;
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.releasedWhenClosed = NO;
    w.title = au_cocoa_dlg_str(au_list_title(kind));
    w.minSize = NSMakeSize(360, 200);
    w.delegate = lw;
    lw->win = w;
    NSView *content = w.contentView;

    AuListTable *table = [[AuListTable alloc] initWithFrame:NSMakeRect(0, 0, W, H - BAR_H)];
    table.owner = lw;
    table.usesAlternatingRowBackgroundColors = YES;
    table.allowsMultipleSelection = NO;
    table.allowsEmptySelection = YES;
    table.allowsColumnReordering = NO;
    table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    table.gridStyleMask = NSTableViewSolidVerticalGridLineMask;
    table.floatsGroupRows = YES;
    table.rowHeight = 20;
    if (@available(macOS 11.0, *))
        table.style = NSTableViewStyleFullWidth;
    const char *const *headings;
    int ncols = au_list_columns(kind, &headings);
    for (int i = 0; i < ncols && i < AU_LIST_MAX_COLS; i++) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%d", i]];
        c.title = au_cocoa_dlg_str(headings[i]);
        c.resizingMask = NSTableColumnUserResizingMask | (i == 0 ? NSTableColumnAutoresizingMask : 0);
        c.width = i == 0 ? 220 : 120;
        c.minWidth = 40;
        [table addTableColumn:c];
    }
    table.dataSource = lw;
    table.delegate = lw;
    table.target = lw;
    table.doubleAction = @selector(doubleClicked:);
    lw->table = table;

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, BAR_H, W, H - BAR_H)];
    sv.hasVerticalScroller = YES;
    sv.hasHorizontalScroller = YES;
    sv.autohidesScrollers = YES;
    sv.borderType = NSNoBorder;
    sv.documentView = table;
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:sv];

    NSBox *sep = [[NSBox alloc] initWithFrame:NSMakeRect(0, BAR_H - 1, W, 1)];
    sep.boxType = NSBoxSeparator;
    sep.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:sep];

    NSStackView *bar = [NSStackView new];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;
    const AuListButton *b;
    int nb = au_list_buttons(kind, &b);
    for (int i = 0; i < nb && i < AU_LIST_ACTIONS; i++) {
        NSString *label = au_cocoa_dlg_str(b[i].label);
        if ([label hasSuffix:@"..."])
            label = [[label substringToIndex:label.length - 3] stringByAppendingString:@"…"];
        NSButton *btn = [NSButton buttonWithTitle:label target:lw action:@selector(buttonClicked:)];
        btn.tag = i;
        [bar addArrangedSubview:btn];
        lw->buttons[i] = btn;
        lw->actions[i] = b[i].action;
        lw->nbuttons++;
    }
    NSSize fit = bar.fittingSize;
    bar.frame = NSMakeRect(12, (BAR_H - fit.height) / 2, fit.width, fit.height);
    bar.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:bar];

    g_lists[kind] = lw;
    [lw syncView];
    __weak AuListWin *weak = lw;
    lw->timer = [NSTimer scheduledTimerWithTimeInterval:0.25
                                                repeats:YES
                                                  block:^(NSTimer *t) {
                                                    [weak tick:t];
                                                  }];
    [w center];
    [w makeKeyAndOrderFront:nil];
    [w makeFirstResponder:table];
    au_cocoa_dlg_windows_changed();
}

void au_cocoa_list_touch(AuListKind kind)
{
    if ((int)kind >= 0 && kind < AU_LIST_KINDS && g_lists[kind] && !g_lists[kind]->closed)
        au_list_touch(&g_lists[kind]->m);
}

/* The open list whose window is w and whose selected row Copy can take. */
static AuListWin *copy_source(NSWindow *w)
{
    for (int i = 0; i < AU_LIST_KINDS; i++) {
        AuListWin *lw = g_lists[i];
        if (lw && !lw->closed && lw->win == w && lw->m.selected >= 0 && lw->m.selected < lw->m.n &&
            lw->m.items[lw->m.selected].type != AU_LIST_ITEM_GROUP)
            return lw;
    }
    return nil;
}

bool au_cocoa_list_can_copy(NSWindow *w) { return w && copy_source(w) != nil; }

bool au_cocoa_list_copy(NSWindow *w)
{
    AuListWin *lw = w ? copy_source(w) : nil;
    if (!lw)
        return false;
    const AuListItem *it = &lw->m.items[lw->m.selected];
    const char *const *headings;
    int ncols = au_list_columns(lw->m.kind, &headings);
    if (ncols > AU_LIST_MAX_COLS)
        ncols = AU_LIST_MAX_COLS;
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (int c = 0; c < ncols; c++)
        if (it->cols[c] && it->cols[c][0])
            [parts addObject:au_cocoa_dlg_str(it->cols[c])];
    NSString *s = [parts componentsJoinedByString:@"\t"];
    const char *u = s.UTF8String;
    return u && au_cocoa_dlg_copy(u, strlen(u));
}

void au_cocoa_lists_shutdown(void)
{
    for (int i = 0; i < AU_LIST_KINDS; i++)
        if (g_lists[i])
            [g_lists[i]->win close];
}

bool au_cocoa_lists_built(void) { return true; }
