/* The menu bar from the command table (browser/commands.h), the
 * AuCommandHost, contextual menus and the Edit commands (docs/cocoa.md,
 * "Menus").
 *
 * Every command item runs runCommand: with its AuCommand as the tag; its
 * enabled and checked state come from au_command_enabled and
 * au_command_checked as the menu is shown or a shortcut is looked up. The
 * bar is laid out as Mac applications lay theirs out: the application
 * menu holds About, Preferences (Command-comma), the Services and Hide
 * items and Quit, which the table places in Edit and File; the Window
 * menu begins with Minimize and Zoom. The Go, Bookmarks and Window menus
 * end with the model's dynamic list, rebuilt when the menu opens. */
#import "cocoa.h"
#import "windows_cocoa.h"

#include <string.h>

/* "Open File..." shows as "Open File…". */
static NSString *label_of(const char *title)
{
    NSString *s = au_cocoa_str(title ? title : "");
    if ([s hasSuffix:@"..."])
        s = [[s substringToIndex:s.length - 3] stringByAppendingString:@"…"];
    return s;
}

/* ---- State and running ---- */

static NSWindow *key_window(void)
{
    return NSApp.keyWindow;
}

void au_cocoa_command_state(AuCommandState *st)
{
    memset(st, 0, sizeof *st);
    st->browser = au_cocoa.browser;
    AuCocoaWin *w = au_cocoa_front();
    st->window = w ? w->bw : NULL;
    NSWindow *key = key_window();
    if (!key)
        st->front = w ? AU_FRONT_BROWSER : AU_FRONT_NONE;
    else if (w && key == w->window)
        st->front = AU_FRONT_BROWSER;
    else if (au_cocoa_devwin_owns(key))
        st->front = AU_FRONT_TOOL;
    else
        st->front = AU_FRONT_OTHER;
    st->text_focus = [key.firstResponder isKindOfClass:[NSText class]];
    if (st->front == AU_FRONT_TOOL) {
        /* A developer window takes Copy, Paste and Select All itself
         * (au_cocoa_edit), and Find and Find Again. */
        st->text_focus = true;
        st->tool_can_find_again = au_cocoa_devwin_can_find_again(key);
    } else if (st->front == AU_FRONT_OTHER && !st->text_focus) {
        st->other_can_copy = au_cocoa_list_can_copy(key);
    }
    st->mods = au_cocoa_current_mods();
}

/* The command a shortcut-bearing item runs: its own, or what its key
 * means now (Close Tab's key closes a window without tabs). */
static AuCommand resolve(const AuCommandState *st, AuCommand cmd)
{
    const AuCommandInfo *ci = au_command_info(cmd);
    if (au_command_enabled(st, cmd) || !ci || !ci->shortcut.key)
        return cmd;
    return au_command_for_key(st, ci->shortcut.key, ci->shortcut.mods);
}

void au_cocoa_run(AuCommand cmd)
{
    if (!au_cocoa.started || au_cocoa.quitting || au_cocoa.modal)
        return;
    AuCommandState st;
    au_cocoa_command_state(&st);
    au_command_run(&st, au_cocoa_command_host(), resolve(&st, cmd));
    au_cocoa_kick();
}

/* ---- The menu controller ---- */

@interface AuMenus : NSObject <NSMenuDelegate, NSMenuItemValidation> {
  @public
    NSMenu *dyn_menu[3];
    AuMenuDynamic dyn_kind[3];
    NSInteger dyn_start[3]; /* the index of the first dynamic item */
    AuMenuList dyn_list[3];
    int ndyn;
    bool dirty;
    AuContextMenu *ctx;  /* the contextual menu being shown */
    AuCocoaWin *ctx_win;
}
@end

static AuMenus *M;

@implementation AuMenus

- (void)runCommand:(NSMenuItem *)item
{
    au_cocoa_run((AuCommand)item.tag);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(runCommand:)) {
        if (!au_cocoa.started || au_cocoa.quitting || au_cocoa.modal)
            return NO;
        AuCommandState st;
        au_cocoa_command_state(&st);
        AuCommand c = (AuCommand)item.tag, r = resolve(&st, c);
        if (au_command_checkable(c))
            item.state = au_command_checked(&st, c) ? NSControlStateValueOn : NSControlStateValueOff;
        return r != AU_CMD_NONE && au_command_enabled(&st, r);
    }
    if (item.action == @selector(runEntry:)) {
        int d = (int)(item.tag >> 16), i = (int)(item.tag & 0xFFFF);
        return !au_cocoa.modal && d < ndyn && i < dyn_list[d].n && dyn_list[d].e[i].enabled;
    }
    if (item.action == @selector(runContext:))
        return !au_cocoa.modal && ctx;
    return YES;
}

- (void)runEntry:(NSMenuItem *)item
{
    int d = (int)(item.tag >> 16), i = (int)(item.tag & 0xFFFF);
    if (!au_cocoa.started || au_cocoa.quitting || au_cocoa.modal || d >= ndyn || i >= dyn_list[d].n)
        return;
    AuCommandState st;
    au_cocoa_command_state(&st);
    au_menu_entry_run(&st, au_cocoa_command_host(), &dyn_list[d].e[i]);
    au_cocoa_kick();
}

- (void)runContext:(NSMenuItem *)item
{
    AuCocoaWin *w = ctx_win;
    if (!ctx || !w || w->freed || au_cocoa.modal || !au_cocoa.started || au_cocoa.quitting)
        return;
    AuCommandState st;
    au_cocoa_command_state(&st);
    st.window = w->bw;
    au_command_run_context(&st, au_cocoa_command_host(), ctx, (AuCommand)item.tag);
    au_cocoa_kick();
}

- (void)bringAllToFront:(id)s
{
    (void)s;
    [NSApp arrangeInFront:nil];
}

/* The Window menu's windows: the browser windows front to back, then the
 * application's other visible windows. A browser window's ref is its
 * AuCocoaWin, any other's its NSWindow. */
static int window_refs(AuWindowRef *refs, int cap)
{
    int n = 0;
    AuCocoaWin *front = au_cocoa_front();
    for (int pass = 0; pass < 2; pass++) {
        for (AuCocoaWin *w in au_cocoa_windows()) {
            if (n >= cap || w->closing || (pass == 0) != (w == front))
                continue;
            refs[n].title = w->window.title.UTF8String;
            refs[n].window = (__bridge void *)w;
            n++;
        }
    }
    for (NSWindow *nw in NSApp.orderedWindows) {
        if (n >= cap)
            break;
        if (au_cocoa_win_for(nw) || !nw.visible || [nw isKindOfClass:[NSPanel class]] || !nw.title.length)
            continue;
        refs[n].title = nw.title.UTF8String;
        refs[n].window = (__bridge void *)nw;
        n++;
    }
    return n;
}

static void fill_list(int d)
{
    NSMenu *menu = M->dyn_menu[d];
    while (menu.numberOfItems > M->dyn_start[d])
        [menu removeItemAtIndex:menu.numberOfItems - 1];
    AuMenuList *l = &M->dyn_list[d];
    if (!l->n)
        return;
    /* The submenu each folder entry opens, by entry index. */
    NSMutableDictionary<NSNumber *, NSMenu *> *menus = [NSMutableDictionary dictionary];
    for (int i = 0; i < l->n && i < 0xFFFF; i++) {
        const AuMenuEntry *e = &l->e[i];
        NSMenu *folder = e->parent >= 0 && e->parent < i ? menus[@(e->parent)] : nil;
        NSMenu *into = folder ? folder : menu;
        if (e->kind == AU_ENTRY_SEPARATOR) {
            if (into.numberOfItems && !into.itemArray.lastObject.separatorItem)
                [into addItem:NSMenuItem.separatorItem];
            continue;
        }
        NSString *title = au_cocoa_str(e->title && *e->title ? e->title : " ");
        if (e->kind == AU_ENTRY_FOLDER) {
            NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
            menus[@(i)] = sub;
            NSMenuItem *it = [into addItemWithTitle:title action:NULL keyEquivalent:@""];
            it.submenu = sub;
            continue;
        }
        NSMenuItem *it = [into addItemWithTitle:title action:@selector(runEntry:) keyEquivalent:@""];
        it.target = M;
        it.tag = ((NSInteger)d << 16) | i;
        it.state = e->checked ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (!au_cocoa.browser)
        return;
    for (int d = 0; d < ndyn; d++) {
        if (dyn_menu[d] != menu)
            continue;
        if (dyn_kind[d] == AU_MENU_LIST_WINDOWS) {
            AuWindowRef refs[64];
            int n = window_refs(refs, 64);
            au_menu_list_free(&dyn_list[d]);
            au_menu_list_windows(refs, n, n ? refs[0].window : NULL, &dyn_list[d]);
        } else {
            au_menu_list_free(&dyn_list[d]);
            if (dyn_kind[d] == AU_MENU_LIST_HISTORY)
                au_menu_list_history(au_cocoa.browser, &dyn_list[d]);
            else
                au_menu_list_bookmarks(au_cocoa.browser, &dyn_list[d]);
        }
        fill_list(d);
    }
}

@end

/* ---- Building ---- */

static void set_key(NSMenuItem *it, const AuShortcut *s)
{
    if (!s->key)
        return;
    char c = s->key >= 'A' && s->key <= 'Z' ? (char)(s->key - 'A' + 'a') : s->key;
    it.keyEquivalent = [NSString stringWithFormat:@"%c", c];
    NSEventModifierFlags m = 0;
    if (s->mods & AU_SHORTCUT_PRIMARY)
        m |= NSEventModifierFlagCommand;
    if (s->mods & AU_SHORTCUT_SHIFT)
        m |= NSEventModifierFlagShift;
    if (s->mods & AU_SHORTCUT_ALT)
        m |= NSEventModifierFlagOption;
    it.keyEquivalentModifierMask = m;
}

static NSMenuItem *command_item(AuCommand cmd)
{
    const AuCommandInfo *ci = au_command_info(cmd);
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:label_of(ci ? ci->title : "")
                                                action:@selector(runCommand:)
                                         keyEquivalent:@""];
    it.target = M;
    it.tag = cmd;
    if (ci)
        set_key(it, &ci->shortcut);
    return it;
}

/* Commands the application menu holds on a Mac, left out of the menus the
 * table puts them in. */
static bool moved_to_app_menu(AuCommand c)
{
    return c == AU_CMD_EDIT_PREFERENCES || c == AU_CMD_FILE_QUIT || c == AU_CMD_ABOUT;
}

static void trim_separators(NSMenu *menu)
{
    while (menu.numberOfItems && menu.itemArray.lastObject.separatorItem)
        [menu removeItemAtIndex:menu.numberOfItems - 1];
}

static NSMenu *build_menu(AuMenuId id)
{
    const AuMenuInfo *mi = au_menu_info(id);
    NSMenu *menu = [[NSMenu alloc] initWithTitle:au_cocoa_str(mi ? mi->title : "")];
    menu.autoenablesItems = YES;
    for (int i = 0; mi && i < mi->nitems; i++) {
        AuCommand cmd = mi->items[i];
        if (cmd == AU_CMD_SEPARATOR) {
            if (menu.numberOfItems && !menu.itemArray.lastObject.separatorItem)
                [menu addItem:NSMenuItem.separatorItem];
            continue;
        }
        if (moved_to_app_menu(cmd))
            continue;
        const AuCommandInfo *ci = au_command_info(cmd);
        if (!ci)
            continue;
        if (ci->submenu != AU_MENU_NONE) {
            NSMenuItem *it = [menu addItemWithTitle:label_of(ci->title) action:NULL keyEquivalent:@""];
            it.submenu = build_menu(ci->submenu);
            continue;
        }
        [menu addItem:command_item(cmd)];
        if (ci->shortcut.key == '+') {
            /* Zoom In: Command and the = key that + shares on most layouts. */
            NSMenuItem *alt = command_item(cmd);
            alt.keyEquivalent = @"=";
            alt.hidden = YES;
            alt.allowsKeyEquivalentWhenHidden = YES;
            [menu addItem:alt];
        }
    }
    trim_separators(menu);
    if (mi && mi->list != AU_MENU_LIST_NONE && M->ndyn < 3) {
        int d = M->ndyn++;
        M->dyn_menu[d] = menu;
        M->dyn_kind[d] = mi->list;
        M->dyn_start[d] = menu.numberOfItems;
        menu.delegate = M;
    }
    return menu;
}

static NSMenu *app_menu(void)
{
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Aura"];
    [m addItem:command_item(AU_CMD_ABOUT)];
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *prefs = command_item(AU_CMD_EDIT_PREFERENCES);
    prefs.keyEquivalent = @",";
    prefs.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [m addItem:prefs];
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *services = [m addItemWithTitle:@"Services" action:NULL keyEquivalent:@""];
    services.submenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu = services.submenu;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Hide Aura" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [m addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:)
                               keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [m addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    NSMenuItem *quit = command_item(AU_CMD_FILE_QUIT);
    quit.title = @"Quit Aura";
    [m addItem:quit];
    return m;
}

void au_cocoa_menus_init(void)
{
    if (M)
        return;
    M = [AuMenus new];
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
    int n;
    const AuMenuId *ids = au_menu_bar(&n);
    for (int i = 0; i <= n; i++) {
        /* The bar's menus, then Help, the last menu on a Mac. */
        AuMenuId id = i < n ? ids[i] : AU_MENU_HELP;
        const AuMenuInfo *mi = au_menu_info(id);
        if (!mi)
            continue;
        NSMenu *menu;
        if (id == AU_MENU_APPLICATION) {
            menu = app_menu();
        } else if (id == AU_MENU_WINDOW) {
            menu = [[NSMenu alloc] initWithTitle:@"Window"];
            [menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
            [menu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
            NSMenuItem *front = [menu addItemWithTitle:@"Bring All to Front" action:@selector(bringAllToFront:)
                                         keyEquivalent:@""];
            front.target = M;
            [menu addItem:NSMenuItem.separatorItem];
            NSMenu *rest = build_menu(id);
            /* Move the table's items (and the dynamic list's start) over. */
            int d = M->ndyn - 1;
            NSInteger base = menu.numberOfItems;
            while (rest.numberOfItems) {
                NSMenuItem *it = rest.itemArray.firstObject;
                [rest removeItem:it];
                [menu addItem:it];
            }
            if (d >= 0 && M->dyn_menu[d] == rest) {
                M->dyn_menu[d] = menu;
                M->dyn_start[d] += base;
                menu.delegate = M;
            }
        } else {
            menu = build_menu(id);
        }
        NSMenuItem *top = [bar addItemWithTitle:menu.title action:NULL keyEquivalent:@""];
        top.submenu = menu;
        if (id == AU_MENU_HELP)
            NSApp.helpMenu = menu;
    }
    NSApp.mainMenu = bar;
}

void au_cocoa_menus_dirty(void)
{
    if (M)
        M->dirty = true;
}

bool au_cocoa_show_window(AuWindowKind kind)
{
    return au_cocoa_window_kind(kind);
}

/* ---- AuCommandHost ---- */

static AuBrowserWindow *h_new_window(void *ctx, const char *url)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_new_window(url, NULL);
    return w ? w->bw : NULL;
}

static void h_raise(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_win_of(bw);
    if (w)
        [w->window makeKeyAndOrderFront:nil];
}

static void h_focus_address(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_win_of(bw);
    if (w)
        au_cocoa_win_focus_address(w);
}

static bool h_show_window(void *ctx, AuWindowKind kind)
{
    (void)ctx;
    return au_cocoa_show_window(kind);
}

static char *h_choose_file_url(void *ctx)
{
    (void)ctx;
    if (au_cocoa.modal)
        return NULL;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    au_cocoa_modal_begin();
    NSModalResponse r = [p runModal];
    au_cocoa_modal_end();
    if (r != NSModalResponseOK || !p.URL)
        return NULL;
    const char *u = p.URL.absoluteString.UTF8String;
    return au_strndup(u, strlen(u));
}

static void h_save_page(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_win_of(bw);
    if (!w || au_cocoa.modal)
        return;
    char name[256];
    au_bw_save_name(bw, name, sizeof name);
    NSSavePanel *p = [NSSavePanel savePanel];
    p.nameFieldStringValue = au_cocoa_str(name);
    au_cocoa_modal_begin();
    NSModalResponse r = [p runModal];
    au_cocoa_modal_end();
    if (r != NSModalResponseOK || !p.URL || w->freed)
        return;
    const char *data;
    size_t len;
    if (!au_bw_page_source(bw, &data, &len))
        return;
    NSError *err = nil;
    if (![[NSData dataWithBytes:data length:len] writeToURL:p.URL options:NSDataWritingAtomic error:&err])
        au_cocoa_notice(w->window, "The page could not be saved.", err.localizedDescription.UTF8String);
}

static void h_close_front(void *ctx)
{
    (void)ctx;
    NSWindow *key = key_window();
    AuCocoaWin *w = au_cocoa_front();
    if (key)
        [key performClose:nil];
    else if (w)
        [w->window performClose:nil];
}

static void h_find(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuCocoaWin *w = bw ? au_cocoa_win_of(bw) : nil;
    NSWindow *key = key_window();
    if (w)
        au_cocoa_win_find(w);
    else if (key && au_cocoa_devwin_owns(key))
        au_cocoa_devwin_find(key);
}

static void h_tool_find_again(void *ctx, bool forward)
{
    (void)ctx;
    NSWindow *key = key_window();
    if (key && au_cocoa_devwin_owns(key))
        au_cocoa_devwin_find_again(key, forward);
}

static void h_edit(void *ctx, AuEditAction a)
{
    (void)ctx;
    au_cocoa_edit(a);
}

static void h_copy_text(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    au_cocoa_copy_text(utf8, len);
}

static void h_select_window(void *ctx, void *window)
{
    (void)ctx;
    for (AuCocoaWin *w in au_cocoa_windows()) {
        if ((__bridge void *)w == window) {
            [w->window makeKeyAndOrderFront:nil];
            return;
        }
    }
    /* Another window: only while the application still has it. */
    for (NSWindow *nw in NSApp.windows)
        if ((__bridge void *)nw == window)
            [nw makeKeyAndOrderFront:nil];
}

static void h_beep(void *ctx)
{
    (void)ctx;
    NSBeep();
}

static void h_quit(void *ctx)
{
    (void)ctx;
    /* After the command returns: quitting frees the model. */
    dispatch_async(dispatch_get_main_queue(), ^{
        au_cocoa_quit();
    });
}

static const AuCommandHost command_host = {
    NULL,          h_new_window,  h_raise,     h_focus_address,  h_show_window,
    h_choose_file_url, h_save_page, h_close_front, h_find,      h_tool_find_again,
    h_edit,        h_copy_text,   h_select_window, h_beep,      h_quit};

const AuCommandHost *au_cocoa_command_host(void)
{
    return &command_host;
}

/* ---- Editing ---- */

static NSString *pasteboard_text(void)
{
    return [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
}

void au_cocoa_edit(AuEditAction a)
{
    AuCocoaWin *w = au_cocoa_front();
    if (a == AU_EDIT_PASTE_AND_GO) {
        /* Paste and Go: one line, trimmed, opened from the page. */
        NSString *s = pasteboard_text();
        if (!w || w->freed || !s)
            return;
        s = [[s componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]
            componentsJoinedByString:@" "];
        s = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (s.length) {
            au_bw_set_field_text(w->bw, NULL);
            [w->window makeFirstResponder:w->page];
            au_bw_navigate_input(w->bw, s.UTF8String);
        }
        au_cocoa_kick();
        return;
    }
    NSWindow *key = key_window();
    if ([key.firstResponder isKindOfClass:[NSText class]]) {
        /* The address field, the find field, or a dialog's field. */
        static SEL sels[6];
        sels[0] = @selector(undo:);
        sels[1] = @selector(cut:);
        sels[2] = @selector(copy:);
        sels[3] = @selector(paste:);
        sels[4] = @selector(delete:);
        sels[5] = @selector(selectAll:);
        if ((size_t)a < 6)
            [NSApp sendAction:sels[a] to:nil from:nil];
        return;
    }
    if (key && au_cocoa_devwin_owns(key)) {
        au_cocoa_devwin_edit(key, a);
        return;
    }
    if (key && au_cocoa_list_can_copy(key)) {
        if (a == AU_EDIT_COPY)
            au_cocoa_list_copy(key);
        return;
    }
    if (!w || w->freed)
        return;
    if (au_bw_page_editing(w->bw)) {
        static const int page_cmd[] = {AU_PAGE_EDIT_UNDO,  AU_PAGE_EDIT_CUT,   AU_PAGE_EDIT_COPY,
                                       AU_PAGE_EDIT_PASTE, AU_PAGE_EDIT_CLEAR, AU_PAGE_EDIT_SELECT_ALL};
        if ((size_t)a >= sizeof page_cmd / sizeof page_cmd[0])
            return;
        const char *paste = NULL;
        size_t plen = 0;
        NSString *ps = nil;
        if (a == AU_EDIT_PASTE) {
            ps = pasteboard_text();
            if (!ps)
                return;
            paste = ps.UTF8String;
            plen = strlen(paste);
        }
        AuBuf out;
        au_buf_init(&out);
        if (au_bw_page_edit(w->bw, page_cmd[a], paste, plen, &out) && out.len &&
            (a == AU_EDIT_CUT || a == AU_EDIT_COPY))
            au_cocoa_copy_text(out.data, out.len);
        au_buf_free(&out);
    } else if (a == AU_EDIT_COPY) {
        AuBuf out;
        au_buf_init(&out);
        if (au_bw_selection_text(w->bw, &out) && out.len)
            au_cocoa_copy_text(out.data, out.len);
        au_buf_free(&out);
    } else if (a == AU_EDIT_SELECT_ALL) {
        au_bw_select_all(w->bw);
    }
    au_cocoa_kick();
}

/* ---- Contextual menus ---- */

void au_cocoa_context_menu(AuCocoaWin *w, NSEvent *ev, double x, double y)
{
    if (w->have_menu) {
        au_context_menu_free(&w->menu);
        w->have_menu = false;
    }
    unsigned flags = 0;
    if (pasteboard_text().length)
        flags |= AU_CONTEXT_CLIPBOARD_TEXT;
    memset(&w->menu, 0, sizeof w->menu);
    if (!au_bw_context_menu(w->bw, (int)x, (int)y, flags, &w->menu) || !w->menu.n) {
        au_context_menu_free(&w->menu);
        return;
    }
    w->have_menu = true;
    AuCommandState st;
    au_cocoa_command_state(&st);
    st.window = w->bw;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    for (int i = 0; i < w->menu.n; i++) {
        const AuContextItem *it = &w->menu.items[i];
        if (it->cmd == AU_CMD_SEPARATOR) {
            if (menu.numberOfItems && !menu.itemArray.lastObject.separatorItem)
                [menu addItem:NSMenuItem.separatorItem];
            continue;
        }
        const char *title = it->label ? it->label : au_command_title(&st, it->cmd);
        NSMenuItem *mi = [menu addItemWithTitle:label_of(title) action:@selector(runContext:) keyEquivalent:@""];
        mi.target = M;
        mi.tag = it->cmd;
        mi.enabled = it->enabled;
    }
    trim_separators(menu);
    /* Kept until the next contextual menu: the item's action may come
     * after popUpContextMenu returns. runContext: checks w->freed. */
    M->ctx = &w->menu;
    M->ctx_win = w;
    [NSMenu popUpContextMenu:menu withEvent:ev forView:w->page];
}
