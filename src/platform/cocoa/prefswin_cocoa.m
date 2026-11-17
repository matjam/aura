/* The Preferences window.
 *
 * Every item, its kind, caption, choices, enabling rule, status line and
 * what a change does is the edit session in src/browser/prefs_ui.c. This
 * file builds the window from the descriptions alone and holds only what
 * is about AppKit:
 *   - a preferences-style toolbar with one selectable item per section,
 *     and a tabless NSTabView that shows the chosen section; on a page an
 *     NSGridView of caption and control, one control per item by its kind
 *     (check box, radio pair, number field with its unit, text field,
 *     pop-up menu, folder field with Choose, button, note, the blocking
 *     lists' rows);
 *   - turning a change into the session's setter: check boxes, radio
 *     buttons and pop-up menus at once (live items applied to the
 *     browser), text and number fields when OK is clicked, as on Mac OS 9;
 *   - OK (commit), Cancel and closing the window (cancel), Restore
 *     Defaults (the visible section), and the buttons' actions;
 *   - a timer that redraws the blocking lists' status lines while they
 *     download. */
#import "windows_cocoa.h"

#include <string.h>

static const AuPrefDesc *desc(AuPrefId id) { return au_pref_desc(id); }

static NSString *str(const char *utf8)
{
    NSString *s = utf8 ? [NSString stringWithUTF8String:utf8] : nil;
    return s ? s : @"";
}

static uint32_t memory_free_kb(void)
{
    return au_cocoa_wh.memory_free_kb ? au_cocoa_wh.memory_free_kb(au_cocoa_wh.ctx) : 0;
}

static AuDefaultBrowser default_browser(void)
{
    if (!au_cocoa_wh.default_browser || !au_cocoa_wh.make_default_browser)
        return AU_DEFAULT_BROWSER_UNAVAILABLE;
    return au_cocoa_wh.default_browser(au_cocoa_wh.ctx);
}

static void blocking_changed(void)
{
    if (au_cocoa_wh.blocking_changed)
        au_cocoa_wh.blocking_changed(au_cocoa_wh.ctx);
}

static void windows_changed(void)
{
    if (au_cocoa_wh.windows_changed)
        au_cocoa_wh.windows_changed(au_cocoa_wh.ctx);
}

/* The SF Symbol of a section's toolbar item. */
static NSString *section_symbol(AuPrefSection s)
{
    switch (s) {
    case AU_PREF_SECTION_GENERAL: return @"gearshape";
    case AU_PREF_SECTION_FONTS: return @"textformat";
    case AU_PREF_SECTION_CONTENT: return @"doc.richtext";
    case AU_PREF_SECTION_PRIVACY: return @"hand.raised";
    case AU_PREF_SECTION_NETWORK: return @"network";
    case AU_PREF_SECTION_DOWNLOADS: return @"arrow.down.circle";
    case AU_PREF_SECTION_BLOCKING: return @"shield.lefthalf.filled";
    default: return @"gearshape";
    }
}

static NSToolbarItemIdentifier section_ident(AuPrefSection s)
{
    return [NSString stringWithFormat:@"aura.prefs.%d", (int)s];
}

/* A page's document view: flipped, so a short page sits at the top. */
@interface AuPrefsPage : NSView
@end
@implementation AuPrefsPage
- (BOOL)isFlipped
{
    return YES;
}
@end

@interface AuPrefsWindow : NSObject <NSWindowDelegate, NSToolbarDelegate> {
  @public
    AuPrefsEdit *edit;
    NSWindow *win;
    NSTabView *tabs;
    NSControl *ctl[AU_PREF_COUNT];    /* the control; a radio item's first button */
    NSControl *ctl2[AU_PREF_COUNT];   /* a radio item's second button; a folder's Choose button */
    NSTextField *status[AU_PREF_COUNT]; /* the status line or note text */
    NSGridRow *statusRow[AU_PREF_COUNT]; /* the grid row the status line or note is on */
    NSMutableArray<NSButton *> *blockCheck;
    NSMutableArray<NSTextField *> *blockStatus;
    size_t nblock;
    uint32_t blockGen;
    bool loading; /* controls are being set from the session */
    bool done;    /* OK or Cancel already ran */
    NSTimer *timer;
}
@end

static AuPrefsWindow *g_prefs;

/* ---- Values ---- */

static void load_item(AuPrefsWindow *p, AuPrefId id)
{
    NSControl *c = p->ctl[id];
    if (!c)
        return;
    p->loading = true;
    switch (desc(id)->kind) {
    case AU_PREF_CHECK:
        ((NSButton *)c).state = au_prefs_edit_get_bool(p->edit, id) ? NSControlStateValueOn : NSControlStateValueOff;
        break;
    case AU_PREF_RADIO: {
        int v = au_prefs_edit_get_choice(p->edit, id);
        ((NSButton *)c).state = v == 1 ? NSControlStateValueOff : NSControlStateValueOn;
        ((NSButton *)p->ctl2[id]).state = v == 1 ? NSControlStateValueOn : NSControlStateValueOff;
        break;
    }
    case AU_PREF_NUMBER:
    case AU_PREF_TEXT:
    case AU_PREF_FOLDER: {
        AuBuf b;
        au_buf_init(&b);
        au_prefs_edit_get_text(p->edit, id, &b);
        c.stringValue = str(au_buf_cstr(&b));
        au_buf_free(&b);
        break;
    }
    case AU_PREF_CHOICE:
    case AU_PREF_FONT: {
        NSPopUpButton *pop = (NSPopUpButton *)c;
        int v = au_prefs_edit_get_choice(p->edit, id);
        if (v < 0 && desc(id)->kind == AU_PREF_FONT) {
            /* A family that is not installed: shown after the installed
             * ones, so the menu does not claim the first of them. */
            NSInteger n = (NSInteger)au_prefs_edit_choice_count(p->edit, id);
            while (pop.menu.numberOfItems > n)
                [pop.menu removeItemAtIndex:n];
            AuBuf b;
            au_buf_init(&b);
            au_prefs_edit_get_text(p->edit, id, &b);
            NSString *name = [NSString stringWithFormat:@"%@ (not installed)", str(au_buf_cstr(&b))];
            au_buf_free(&b);
            [pop.menu addItemWithTitle:name action:NULL keyEquivalent:@""];
            v = (int)n;
        }
        [pop selectItemAtIndex:v >= 0 && v < pop.menu.numberOfItems ? v : -1];
        break;
    }
    default: break;
    }
    p->loading = false;
}

static void load_blocking(AuPrefsWindow *p)
{
    AuPrefBlockRow row;
    p->loading = true;
    for (size_t i = 0; i < p->nblock; i++) {
        if (!au_prefs_edit_block_list(p->edit, i, &row))
            continue;
        p->blockCheck[i].state = row.on ? NSControlStateValueOn : NSControlStateValueOff;
        p->blockStatus[i].stringValue = str(row.status);
    }
    p->blockGen = au_prefs_edit_block_generation(p->edit);
    p->loading = false;
}

/* Status lines and enabled states, after a change. */
static void refresh_states(AuPrefsWindow *p)
{
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        if (p->status[id]) {
            au_buf_clear(&b);
            au_prefs_edit_text(p->edit, id, &b);
            NSString *s = str(au_buf_cstr(&b));
            if (![s isEqualToString:p->status[id].stringValue])
                p->status[id].stringValue = s;
            if (p->statusRow[id])
                p->statusRow[id].hidden = b.len == 0;
        }
        if (!p->ctl[id])
            continue;
        bool on = au_prefs_edit_enabled(p->edit, id);
        p->ctl[id].enabled = on;
        if (p->ctl2[id])
            p->ctl2[id].enabled = on;
    }
    au_buf_free(&b);
}

static void load_all(AuPrefsWindow *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++)
        load_item(p, (AuPrefId)i);
    load_blocking(p);
    refresh_states(p);
}

/* OK: the fields' text into the session (the other kinds are set as they
 * change). */
static void store(AuPrefsWindow *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        if (!p->ctl[id])
            continue;
        AuPrefKind k = desc(id)->kind;
        if (k == AU_PREF_NUMBER || k == AU_PREF_TEXT || k == AU_PREF_FOLDER) {
            const char *text = p->ctl[id].stringValue.UTF8String;
            au_prefs_edit_set_text(p->edit, id, text ? text : "");
        }
    }
}

/* A live item changed in the session: apply it to the browser now. */
static void apply_live(AuPrefsWindow *p)
{
    unsigned bits = au_prefs_edit_apply_live(p->edit, au_cocoa_wh.browser);
    if (bits & AU_PREF_CHANGED_BLOCKING)
        blocking_changed();
    if (bits && au_cocoa_wh.prefs_changed)
        au_cocoa_wh.prefs_changed(au_cocoa_wh.ctx, bits, false);
}

/* After a check box, radio button or pop-up menu changed id. */
static void changed(AuPrefsWindow *p, AuPrefId id)
{
    if (desc(id)->flags & AU_PREF_LIVE)
        apply_live(p);
    /* Automatic script memory follows the memory free now. */
    au_prefs_edit_set_memory_free(p->edit, memory_free_kb());
    refresh_states(p);
}

/* ---- Actions ---- */

static void run_action(AuPrefsWindow *p, AuPrefId id)
{
    AuBrowserWindow *front = au_cocoa_wh.front_window ? au_cocoa_wh.front_window(au_cocoa_wh.ctx) : NULL;
    AuPrefId what = AU_PREF_NONE;
    unsigned todo = au_prefs_edit_action(p->edit, id, au_cocoa_wh.browser, front, &what);
    if ((todo & AU_PREF_DO_RELOAD) && what != AU_PREF_NONE)
        load_item(p, what);
    if (todo & AU_PREF_DO_MAKE_DEFAULT) {
        if (!au_cocoa_wh.make_default_browser || !au_cocoa_wh.make_default_browser(au_cocoa_wh.ctx))
            NSBeep();
        au_prefs_edit_set_default_browser(p->edit, default_browser());
    }
    if (todo & AU_PREF_DO_CLEAR_COOKIES) {
        if (au_cocoa_wh.aura_app)
            au_app_clear_cookies(au_cocoa_wh.aura_app);
        au_cocoa_list_touch(AU_LIST_COOKIES);
    }
    if ((todo & AU_PREF_DO_CLEAR_CACHE) && au_cocoa_wh.aura_app)
        au_app_clear_cache(au_cocoa_wh.aura_app);
    if (todo & AU_PREF_DO_SHOW_COOKIES)
        au_cocoa_list_show(AU_LIST_COOKIES);
    if (todo & AU_PREF_DO_REFRESH_LISTS)
        load_blocking(p);
    refresh_states(p);
}

/* ---- Closing ---- */

static void finish(AuPrefsWindow *p, bool ok)
{
    if (p->done)
        return;
    p->done = true;
    if (ok) {
        store(p);
        au_prefs_edit_commit(p->edit, au_cocoa_wh.browser);
        if (au_cocoa_wh.prefs_changed)
            au_cocoa_wh.prefs_changed(au_cocoa_wh.ctx, 0, true);
    } else {
        unsigned bits = au_prefs_edit_cancel(p->edit, au_cocoa_wh.browser);
        if (bits & AU_PREF_CHANGED_BLOCKING)
            blocking_changed();
        if (bits && au_cocoa_wh.prefs_changed)
            au_cocoa_wh.prefs_changed(au_cocoa_wh.ctx, bits, false);
    }
}

@implementation AuPrefsWindow

- (void)checkChanged:(NSButton *)sender
{
    if (loading)
        return;
    AuPrefId id = (AuPrefId)sender.tag;
    au_prefs_edit_set_bool(edit, id, sender.state == NSControlStateValueOn);
    changed(self, id);
}

- (void)radioChanged:(NSButton *)sender
{
    if (loading || sender.state != NSControlStateValueOn)
        return;
    AuPrefId id = (AuPrefId)sender.tag;
    au_prefs_edit_set_choice(edit, id, sender == ctl2[id] ? 1 : 0);
    changed(self, id);
}

- (void)popupChanged:(NSPopUpButton *)sender
{
    if (loading)
        return;
    AuPrefId id = (AuPrefId)sender.tag;
    NSInteger v = sender.indexOfSelectedItem;
    if (v < 0 || (size_t)v >= au_prefs_edit_choice_count(edit, id))
        return; /* none, or the family that is not installed */
    if (!au_prefs_edit_set_choice(edit, id, (int)v))
        load_item(self, id);
    changed(self, id);
}

- (void)actionClicked:(NSButton *)sender
{
    run_action(self, (AuPrefId)sender.tag);
}

- (void)blockChanged:(NSButton *)sender
{
    if (loading)
        return;
    au_prefs_edit_set_block_list(edit, (size_t)sender.tag, sender.state == NSControlStateValueOn);
    apply_live(self);
    load_blocking(self);
    refresh_states(self);
}

- (void)chooseFolder:(NSButton *)sender
{
    AuPrefId id = (AuPrefId)sender.tag;
    NSTextField *field = (NSTextField *)ctl[id];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Choose a Download Folder";
    panel.message = @"Choose a Download Folder";
    panel.prompt = @"Choose";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = YES;
    NSString *cur = field.stringValue;
    if ([cur hasPrefix:@"/"])
        panel.directoryURL = [NSURL fileURLWithPath:cur isDirectory:YES];
    if (au_cocoa_wh.modal_begin)
        au_cocoa_wh.modal_begin(au_cocoa_wh.ctx);
    NSModalResponse r = [panel runModal];
    if (au_cocoa_wh.modal_end)
        au_cocoa_wh.modal_end(au_cocoa_wh.ctx);
    if (r == NSModalResponseOK && panel.URL && g_prefs == self && ctl[id] == field) {
        const char *path = panel.URL.fileSystemRepresentation;
        if (path)
            field.stringValue = str(path);
    }
}

- (void)okClicked:(id)sender
{
    (void)sender;
    [win endEditingFor:nil];
    finish(self, true);
    [win close];
}

- (void)cancelClicked:(id)sender
{
    (void)sender;
    finish(self, false);
    [win close];
}

- (void)defaultsClicked:(id)sender
{
    (void)sender;
    NSTabViewItem *item = tabs.selectedTabViewItem;
    if (!item)
        return;
    NSInteger s = [tabs indexOfTabViewItem:item];
    if (s < 0 || s >= AU_PREF_SECTION_COUNT)
        return;
    au_prefs_edit_reset_section(edit, (AuPrefSection)s);
    apply_live(self);
    load_all(self);
}

- (void)sectionClicked:(NSToolbarItem *)sender
{
    NSInteger s = sender.tag;
    if (s < 0 || s >= AU_PREF_SECTION_COUNT)
        return;
    [win endEditingFor:nil];
    [tabs selectTabViewItemAtIndex:s];
    win.title = str(au_pref_section_title((AuPrefSection)s));
}

- (void)tick
{
    if (au_prefs_edit_block_generation(edit) != blockGen) {
        load_blocking(self);
        refresh_states(self);
    }
}

/* ---- NSWindowDelegate ---- */

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    finish(self, false); /* closed with the close button: cancel */
    [timer invalidate];
    timer = nil;
    au_prefs_edit_free(edit);
    edit = NULL;
    win.delegate = nil;
    win.toolbar.delegate = nil;
    /* Kept until the window has finished closing. */
    AuPrefsWindow *keep = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        (void)keep;
    });
    if (g_prefs == self)
        g_prefs = nil;
    windows_changed();
}

/* ---- NSToolbarDelegate ---- */

- (NSArray<NSToolbarItemIdentifier> *)sectionIdents
{
    NSMutableArray *a = [NSMutableArray array];
    for (int s = 0; s < AU_PREF_SECTION_COUNT; s++)
        [a addObject:section_ident((AuPrefSection)s)];
    return a;
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return [self sectionIdents];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return [self sectionIdents];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarSelectableItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return [self sectionIdents];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)ident
    willBeInsertedIntoToolbar:(BOOL)flag
{
    (void)toolbar;
    (void)flag;
    for (int s = 0; s < AU_PREF_SECTION_COUNT; s++) {
        if (![ident isEqualToString:section_ident((AuPrefSection)s)])
            continue;
        NSString *title = str(au_pref_section_title((AuPrefSection)s));
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = title;
        item.paletteLabel = title;
        item.toolTip = title;
        item.image = [NSImage imageWithSystemSymbolName:section_symbol((AuPrefSection)s)
                               accessibilityDescription:title];
        item.tag = s;
        item.target = self;
        item.action = @selector(sectionClicked:);
        return item;
    }
    return nil;
}

@end

/* ---- Building ---- */

static NSTextField *small_label(const char *text)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:str(text)];
    l.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    l.textColor = NSColor.secondaryLabelColor;
    l.preferredMaxLayoutWidth = 440;
    l.selectable = NO;
    return l;
}

static void min_width(NSView *v, CGFloat w)
{
    [v.widthAnchor constraintGreaterThanOrEqualToConstant:w].active = YES;
}

static NSStackView *hstack(NSArray<NSView *> *views, CGFloat spacing)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = spacing;
    s.alignment = NSLayoutAttributeFirstBaseline;
    return s;
}

static NSView *build_blocking(AuPrefsWindow *p)
{
    NSStackView *box = [[NSStackView alloc] init];
    box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment = NSLayoutAttributeLeading;
    box.spacing = 4;
    p->nblock = au_prefs_edit_block_list_count(p->edit);
    p->blockCheck = [NSMutableArray array];
    p->blockStatus = [NSMutableArray array];
    AuPrefBlockRow row;
    for (size_t i = 0; i < p->nblock; i++) {
        if (!au_prefs_edit_block_list(p->edit, i, &row))
            memset(&row, 0, sizeof row);
        NSButton *check = [NSButton checkboxWithTitle:str(row.title) target:p action:@selector(blockChanged:)];
        check.tag = (NSInteger)i;
        NSTextField *st = small_label(row.status);
        st.alignment = NSTextAlignmentRight;
        st.preferredMaxLayoutWidth = 260;
        [st setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
        [p->blockCheck addObject:check];
        [p->blockStatus addObject:st];
        NSStackView *line = hstack(@[check, st], 8);
        min_width(line, 440);
        [box addArrangedSubview:line];
    }
    if (!p->nblock)
        [box addArrangedSubview:small_label("The blocking lists are not available yet.")];
    return box;
}

static NSPopUpButton *popup_of(AuPrefsWindow *p, AuPrefId id)
{
    NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    /* Through the menu, which keeps items of the same title. */
    size_t n = au_prefs_edit_choice_count(p->edit, id);
    for (size_t i = 0; i < n; i++)
        [pop.menu addItemWithTitle:str(au_prefs_edit_choice(p->edit, id, i)) action:NULL keyEquivalent:@""];
    if (!n && desc(id)->empty)
        [pop.menu addItemWithTitle:str(desc(id)->empty) action:NULL keyEquivalent:@""];
    pop.target = p;
    pop.action = @selector(popupChanged:);
    return pop;
}

static NSGridRow *add_row(NSGridView *g, NSView *caption, NSView *control)
{
    return [g addRowWithViews:@[caption ? caption : NSGridCell.emptyContentView, control]];
}

/* One item's row(s) of the grid. */
static void build_item(AuPrefsWindow *p, NSGridView *g, AuPrefId id)
{
    const AuPrefDesc *d = desc(id);
    NSView *control = nil; /* what goes in the second column */
    switch (d->kind) {
    case AU_PREF_CHECK:
        control = p->ctl[id] = [NSButton checkboxWithTitle:str(d->title) target:p action:@selector(checkChanged:)];
        break;
    case AU_PREF_RADIO: {
        const char *a = d->choices && d->choices[0] ? d->choices[0] : "";
        const char *b = d->choices && a[0] && d->choices[1] ? d->choices[1] : "";
        /* Radio buttons with one action in one superview are a group. */
        NSButton *r1 = [NSButton radioButtonWithTitle:str(a) target:p action:@selector(radioChanged:)];
        NSButton *r2 = [NSButton radioButtonWithTitle:str(b) target:p action:@selector(radioChanged:)];
        r2.tag = id;
        p->ctl[id] = r1;
        p->ctl2[id] = r2;
        control = hstack(@[r1, r2], 16);
        break;
    }
    case AU_PREF_NUMBER: {
        NSTextField *f = [NSTextField textFieldWithString:@""];
        [f.widthAnchor constraintEqualToConstant:72].active = YES;
        p->ctl[id] = f;
        control = d->unit ? hstack(@[f, [NSTextField labelWithString:str(d->unit)]], 6) : f;
        break;
    }
    case AU_PREF_TEXT: {
        NSTextField *f = [NSTextField textFieldWithString:@""];
        min_width(f, 360);
        control = p->ctl[id] = f;
        break;
    }
    case AU_PREF_FOLDER: {
        NSTextField *f = [NSTextField textFieldWithString:@""];
        min_width(f, 280);
        NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:p action:@selector(chooseFolder:)];
        choose.tag = id;
        p->ctl[id] = f;
        p->ctl2[id] = choose;
        control = hstack(@[f, choose], 6);
        break;
    }
    case AU_PREF_CHOICE:
    case AU_PREF_FONT: control = p->ctl[id] = popup_of(p, id); break;
    case AU_PREF_ACTION:
        control = p->ctl[id] = [NSButton buttonWithTitle:str(d->title) target:p action:@selector(actionClicked:)];
        break;
    case AU_PREF_NOTE: control = p->status[id] = small_label(d->help); break;
    case AU_PREF_LIST: control = build_blocking(p); break;
    }
    if (!control)
        return;
    if (p->ctl[id])
        p->ctl[id].tag = id;
    NSTextField *cap = d->caption ? [NSTextField labelWithString:str(d->caption)] : nil;
    if (cap)
        cap.alignment = NSTextAlignmentRight;
    NSGridRow *row = add_row(g, cap, control);
    if (d->kind == AU_PREF_LIST)
        row.yPlacement = NSGridCellPlacementTop;
    if (d->kind == AU_PREF_NOTE)
        p->statusRow[id] = row;
    if ((d->flags & AU_PREF_STATUS) && d->kind != AU_PREF_NOTE) {
        p->status[id] = small_label("");
        p->statusRow[id] = add_row(g, nil, p->status[id]);
    }
}

static NSView *build_section(AuPrefsWindow *p, AuPrefSection s)
{
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.columnSpacing = 12;
    grid.rowSpacing = 8;
    grid.yPlacement = NSGridCellPlacementCenter;
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        const AuPrefDesc *d = desc((AuPrefId)i);
        if (d && d->section == s)
            build_item(p, grid, (AuPrefId)i);
    }
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;

    AuPrefsPage *page = [[AuPrefsPage alloc] initWithFrame:NSZeroRect];
    page.translatesAutoresizingMaskIntoConstraints = NO;
    [page addSubview:grid];
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 480)];
    sv.hasVerticalScroller = YES;
    sv.hasHorizontalScroller = NO;
    sv.autohidesScrollers = YES;
    sv.drawsBackground = NO;
    sv.borderType = NSNoBorder;
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sv.documentView = page;
    NSClipView *clip = sv.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [page.topAnchor constraintEqualToAnchor:clip.topAnchor],
        [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [page.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
        [grid.topAnchor constraintEqualToAnchor:page.topAnchor constant:20],
        [grid.centerXAnchor constraintEqualToAnchor:page.centerXAnchor],
        [grid.leadingAnchor constraintGreaterThanOrEqualToAnchor:page.leadingAnchor constant:20],
        [page.bottomAnchor constraintEqualToAnchor:grid.bottomAnchor constant:20],
    ]];
    return sv;
}

static AuPrefsEdit *new_session(void)
{
    AuPrefsHost h;
    memset(&h, 0, sizeof h);
    h.fonts = au_cocoa_wh.fonts;
    h.memory_free_kb = memory_free_kb();
    h.blocking = au_cocoa_wh.block_manager ? au_cocoa_wh.block_manager(au_cocoa_wh.ctx) : NULL;
    h.default_browser = default_browser();
    h.machine = au_cocoa_wh.machine ? au_cocoa_wh.machine : "this Mac";
    h.settings_name = au_cocoa_wh.settings_name; /* NULL: the model words it */
    NSString *example = [NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"];
    h.folder_example = au_cocoa_wh.folder_example ? au_cocoa_wh.folder_example : example.UTF8String;
    NSURL *dl = [NSFileManager.defaultManager URLsForDirectory:NSDownloadsDirectory
                                                     inDomains:NSUserDomainMask].firstObject;
    h.system_downloads = au_cocoa_wh.system_downloads ? au_cocoa_wh.system_downloads
                         : dl                         ? dl.lastPathComponent.UTF8String
                                                      : NULL;
    h.no_folder = au_cocoa_wh.no_folder ? au_cocoa_wh.no_folder : "your home folder";
    /* The strings are copied. */
    return au_prefs_edit_new(&au_cocoa_wh.browser->prefs, &h);
}

void au_cocoa_prefs_show(void)
{
    if (g_prefs) {
        [g_prefs->win makeKeyAndOrderFront:nil];
        return;
    }
    if (!au_cocoa_wh.browser)
        return;
    AuPrefsWindow *p = [AuPrefsWindow new];
    p->edit = new_session();
    if (!p->edit)
        return;

    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 540)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    p->win = w;
    w.releasedWhenClosed = NO;
    w.contentMinSize = NSMakeSize(620, 360);
    w.delegate = p;

    p->tabs = [[NSTabView alloc] initWithFrame:NSZeroRect];
    p->tabs.tabViewType = NSNoTabsNoBorder;
    p->tabs.translatesAutoresizingMaskIntoConstraints = NO;
    for (int s = 0; s < AU_PREF_SECTION_COUNT; s++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:section_ident((AuPrefSection)s)];
        item.label = str(au_pref_section_title((AuPrefSection)s));
        item.view = build_section(p, (AuPrefSection)s);
        [p->tabs addTabViewItem:item];
    }

    NSButton *defaults = [NSButton buttonWithTitle:@"Restore Defaults" target:p action:@selector(defaultsClicked:)];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:p action:@selector(cancelClicked:)];
    cancel.keyEquivalent = @"\033";
    NSButton *ok = [NSButton buttonWithTitle:@"OK" target:p action:@selector(okClicked:)];
    /* Return in a field is OK, as on Mac OS 9. */
    ok.keyEquivalent = @"\r";
    [ok.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;
    [cancel.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;
    NSStackView *bar = [[NSStackView alloc] init];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 12;
    bar.edgeInsets = NSEdgeInsetsMake(12, 20, 16, 20);
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addView:defaults inGravity:NSStackViewGravityLeading];
    [bar addView:cancel inGravity:NSStackViewGravityTrailing];
    [bar addView:ok inGravity:NSStackViewGravityTrailing];

    NSBox *line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *content = w.contentView;
    [content addSubview:p->tabs];
    [content addSubview:line];
    [content addSubview:bar];
    [NSLayoutConstraint activateConstraints:@[
        [p->tabs.topAnchor constraintEqualToAnchor:content.topAnchor],
        [p->tabs.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [p->tabs.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [line.topAnchor constraintEqualToAnchor:p->tabs.bottomAnchor],
        [line.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [bar.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [bar.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];

    NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:@"AuraPreferencesToolbar"];
    tb.delegate = p;
    tb.allowsUserCustomization = NO;
    tb.displayMode = NSToolbarDisplayModeIconAndLabel;
    w.toolbar = tb;
    w.toolbarStyle = NSWindowToolbarStylePreference;
    tb.selectedItemIdentifier = section_ident(AU_PREF_SECTION_GENERAL);
    [p->tabs selectTabViewItemAtIndex:0];
    w.title = str(au_pref_section_title(AU_PREF_SECTION_GENERAL));

    g_prefs = p;
    load_all(p);
    __weak AuPrefsWindow *weak = p;
    p->timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                               repeats:YES
                                                 block:^(NSTimer *t) {
                                                     (void)t;
                                                     [weak tick];
                                                 }];
    [[NSRunLoop currentRunLoop] addTimer:p->timer forMode:NSModalPanelRunLoopMode];
    [w center];
    w.frameAutosaveName = @"AuraPreferences"; /* the saved frame, when there is one */
    [w makeKeyAndOrderFront:nil];
    windows_changed();
}

bool au_cocoa_prefs_is_open(void) { return g_prefs != nil; }

void au_cocoa_prefs_shutdown(void)
{
    AuPrefsWindow *p = g_prefs;
    if (!p)
        return;
    finish(p, false);
    [p->win close];
}

bool au_cocoa_prefs_built(void) { return true; }
