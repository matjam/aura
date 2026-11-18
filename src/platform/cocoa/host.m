/* AuBrowserHost for the macOS port, and the dialogs the model waits for.
 *
 * The model asks some questions synchronously (a select's pop-up menu, a
 * confirmation, files for an input type=file, a download warning).
 * AppKit's alerts, panels and pop-up menus can run modally, so each runs
 * in place between au_cocoa_modal_begin and au_cocoa_modal_end. While one
 * runs, the network's sources are off, the tick does nothing and input to
 * browser windows is ignored, so the model is never entered again from
 * inside its own call (docs/cocoa.md, "Dialogs"). */
#import "cocoa.h"
#import "windows_cocoa.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "browser/url.h"
#include "net/loader.h"
#include "net/net.h"

#include <stdio.h>
#include <string.h>

NSString *au_cocoa_str(const char *utf8)
{
    NSString *s = utf8 ? [NSString stringWithUTF8String:utf8] : nil;
    if (!s && utf8)
        s = [[NSString alloc] initWithBytes:utf8 length:strlen(utf8) encoding:NSISOLatin1StringEncoding];
    return s ? s : @"";
}

/* ---- Waiting ---- */

void au_cocoa_modal_begin(void)
{
    if (au_cocoa.modal++ == 0)
        au_cocoa_net_pause(true);
}

void au_cocoa_modal_end(void)
{
    if (--au_cocoa.modal == 0) {
        au_cocoa_net_pause(false);
        au_cocoa_kick();
    }
}

bool au_cocoa_ask(NSWindow *parent, const char *message, const char *detail, const char *yes, const char *no)
{
    (void)parent;
    NSAlert *a = [NSAlert new];
    a.messageText = au_cocoa_str(message);
    if (detail)
        a.informativeText = au_cocoa_str(detail);
    [a addButtonWithTitle:au_cocoa_str(yes)];
    NSButton *cancel = [a addButtonWithTitle:au_cocoa_str(no)];
    cancel.keyEquivalent = @"\033";
    au_cocoa_modal_begin();
    NSModalResponse r = [a runModal];
    au_cocoa_modal_end();
    return r == NSAlertFirstButtonReturn;
}

void au_cocoa_notice(NSWindow *parent, const char *message, const char *detail)
{
    NSAlert *a = [NSAlert new];
    a.messageText = au_cocoa_str(message);
    if (detail)
        a.informativeText = au_cocoa_str(detail);
    if (parent) {
        [a beginSheetModalForWindow:parent completionHandler:nil];
    } else {
        au_cocoa_modal_begin();
        [a runModal];
        au_cocoa_modal_end();
    }
}

void au_cocoa_copy_text(const char *utf8, size_t len)
{
    NSString *s = [[NSString alloc] initWithBytes:utf8 length:len encoding:NSUTF8StringEncoding];
    if (!s)
        return;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
}

static NSWindow *parent_of(AuBrowserWindow *bw)
{
    AuCocoaWin *w = bw ? au_cocoa_win_of(bw) : au_cocoa_front();
    return w ? w->window : nil;
}

/* ---- AuBrowserHost ---- */

static uint32_t h_time_ms(void *ctx)
{
    (void)ctx;
    return au_posix_ms();
}

static AuTime h_now(void *ctx)
{
    (void)ctx;
    return (AuTime)au_posix_wall_time();
}

static bool h_read_file_url(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    char *path = au_url_to_path(url, '/');
    if (!path)
        return false;
    bool ok = au_platform_read_file(path, data, len);
    au_free(path);
    return ok;
}

static void h_invalidate(void *ctx, AuBrowserWindow *bw, AuIRect r)
{
    AuCocoaWin *w = au_cocoa_win_of(bw);
    (void)ctx;
    if (w) {
        au_present_invalidate(&w->present, r);
        au_cocoa_kick();
    }
}

static void h_scroll_area(void *ctx, AuBrowserWindow *bw, AuIRect area, int dx, int dy)
{
    AuCocoaWin *w = au_cocoa_win_of(bw);
    (void)ctx;
    if (w)
        au_present_scroll(&w->present, area, dx, dy);
}

static void h_changed(void *ctx, AuBrowserWindow *bw, unsigned what)
{
    AuCocoaWin *w = au_cocoa_win_of(bw);
    (void)ctx;
    if (w)
        au_cocoa_win_changed(w, what);
    /* History or bookmarks moved: the open History and Bookmarks windows
     * reload soon. */
    if (what & (AU_CHANGED_MENUS | AU_CHANGED_BOOKMARK)) {
        au_cocoa_list_touch(AU_LIST_HISTORY);
        au_cocoa_list_touch(AU_LIST_BOOKMARKS);
    }
}

static void h_close_window(void *ctx, AuBrowserWindow *bw)
{
    AuCocoaWin *w = au_cocoa_win_of(bw);
    (void)ctx;
    /* Freed at the next flush, outside the model's call. */
    if (w) {
        w->closing = true;
        au_cocoa_kick();
    }
}

static void h_copy_text(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    au_cocoa_copy_text(utf8, len);
}

static bool h_open_external(void *ctx, const char *url)
{
    (void)ctx;
    NSURL *u = [NSURL URLWithString:au_cocoa_str(url)];
    return u && [[NSWorkspace sharedWorkspace] openURL:u];
}

/* The Downloads window comes forward when a download starts. */
static void h_download_added(void *ctx, AuDownload *d)
{
    (void)ctx;
    (void)d;
    au_cocoa_list_show(AU_LIST_DOWNLOADS);
}

static bool h_confirm_download(void *ctx, const char *url, const char *name, const char *mime)
{
    (void)ctx;
    (void)url;
    if (!au_cocoa.browser->prefs.warn_app_downloads || !au_cocoa_download_is_app(name, mime))
        return true;
    char msg[512];
    snprintf(msg, sizeof msg, "“%s” is a program.", name);
    return au_cocoa_ask(parent_of(NULL), msg, "Programs from the web can harm your computer. Download it anyway?",
                        "Download", "Cancel");
}

/* The pop-up menu of a select, at the control: the selected option is
 * checked and under the pointer; disabled options and group labels cannot
 * be chosen. */
@interface AuPopupTarget : NSObject {
  @public
    int choice;
}
@end

@implementation AuPopupTarget
- (void)choose:(NSMenuItem *)item
{
    choice = (int)item.tag;
}
@end

static int h_popup(void *ctx, AuBrowserWindow *bw, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_win_of(bw);
    if (!w || n <= 0 || au_cocoa.modal)
        return -1;
    AuPopupTarget *t = [AuPopupTarget new];
    t->choice = -1;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    NSMenuItem *sel = nil;
    for (int i = 0; i < n; i++) {
        NSString *label = au_cocoa_str(items[i].label ? items[i].label : "");
        NSMenuItem *it = [menu addItemWithTitle:label action:@selector(choose:) keyEquivalent:@""];
        it.target = t;
        it.tag = i;
        it.indentationLevel = items[i].indent ? 1 : 0;
        it.enabled = !items[i].disabled && !items[i].group;
        if (items[i].group)
            it.attributedTitle = [[NSAttributedString alloc]
                initWithString:label
                    attributes:@{NSFontAttributeName : [NSFont boldSystemFontOfSize:NSFont.systemFontSize]}];
        if (i == selected) {
            it.state = NSControlStateValueOn;
            sel = it;
        }
    }
    menu.minimumWidth = at.x1 - at.x0;
    au_cocoa_modal_begin();
    [menu popUpMenuPositioningItem:sel atLocation:NSMakePoint(at.x0, sel ? at.y0 : at.y1) inView:w->page];
    au_cocoa_modal_end();
    return t->choice;
}

static bool h_choose_file(void *ctx, bool multiple, AuControlFile **files, uint32_t *n)
{
    (void)ctx;
    if (au_cocoa.modal)
        return false;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = multiple;
    au_cocoa_modal_begin();
    NSModalResponse r = [p runModal];
    au_cocoa_modal_end();
    if (r != NSModalResponseOK || !p.URLs.count)
        return false;
    NSUInteger count = p.URLs.count;
    AuControlFile *out = au_calloc(sizeof *out * count);
    uint32_t got = 0;
    for (NSURL *u in p.URLs) {
        const char *path = u.fileSystemRepresentation;
        char *data;
        size_t len;
        if (!path || !au_platform_read_file(path, &data, &len))
            continue;
        const char *base = u.lastPathComponent.UTF8String;
        UTType *type = [UTType typeWithFilenameExtension:u.pathExtension];
        const char *m = type.preferredMIMEType ? type.preferredMIMEType.UTF8String : "application/octet-stream";
        out[got].name = au_strndup(base, strlen(base));
        out[got].type = au_strndup(m, strlen(m));
        out[got].data = data;
        out[got].len = len;
        got++;
    }
    if (!got) {
        au_free(out);
        return false;
    }
    *files = out;
    *n = got;
    return true;
}

static bool h_confirm(void *ctx, AuBrowserWindow *bw, const char *message)
{
    (void)ctx;
    if (au_cocoa.modal)
        return false;
    return au_cocoa_ask(parent_of(bw), message, NULL, "OK", "Cancel");
}

static void h_notice(void *ctx, AuBrowserWindow *bw, const char *message, const char *detail)
{
    (void)ctx;
    /* A sheet on the window: nothing waits on it */
    NSWindow *parent = parent_of(bw);
    au_cocoa_notice(parent && !parent.attachedSheet ? parent : nil, message, detail);
}

static bool h_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    return au_posix_entropy(buf, len);
}

static uint32_t h_memory_free_kb(void *ctx)
{
    (void)ctx;
    return au_cocoa_memory_free_kb();
}

void au_cocoa_browser_host(AuBrowserHost *h)
{
    memset(h, 0, sizeof *h);
    h->time_ms = h_time_ms;
    h->now = h_now;
    h->read_file_url = h_read_file_url;
    h->invalidate = h_invalidate;
    h->changed = h_changed;
    h->close_window = h_close_window;
    h->open_external = h_open_external;
    h->copy_text = h_copy_text;
    h->confirm_download = h_confirm_download;
    h->download_added = h_download_added;
    h->popup = h_popup;
    h->choose_file = h_choose_file;
    h->confirm = h_confirm;
    h->notice = h_notice;
    h->scroll_area = h_scroll_area;
    h->random = h_random;
    h->coro = au_posix_coro_ops();
    /* on the main thread, where AppKit takes the script's calls: a long
     * task parks and the window scrolls meanwhile */
    h->script_coro = au_posix_ucoro_ops();
    h->memory_free_kb = h_memory_free_kb;
}

/* ---- The secondary windows (windows_cocoa.h) ---- */

static AuBrowserWindow *w_front_window(void *ctx)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa.started && !au_cocoa.quitting ? au_cocoa_front() : nil;
    return w && !w->freed ? w->bw : NULL;
}

static AuPage *w_front_page(void *ctx)
{
    AuBrowserWindow *bw = w_front_window(ctx);
    AuTab *t = bw ? au_bw_active_tab(bw) : NULL;
    return t ? t->page : NULL;
}

static NSWindow *w_front_ns_window(void *ctx)
{
    (void)ctx;
    AuCocoaWin *w = au_cocoa_front();
    return w && !w->freed ? w->window : nil;
}

static void w_open_url(void *ctx, const char *url, bool new_tab)
{
    (void)ctx;
    if (!au_cocoa.started || au_cocoa.quitting || au_cocoa.modal)
        return;
    AuCommandState st;
    au_cocoa_command_state(&st);
    au_command_open_url(&st, au_cocoa_command_host(), url, new_tab);
    AuCocoaWin *w = au_cocoa_front();
    if (w)
        [w->window makeKeyAndOrderFront:nil];
    au_cocoa_kick();
}

/* The Finder shows a finished download in the download folder. */
static bool w_reveal_download(void *ctx, const AuDownload *d)
{
    (void)ctx;
    if (!d->filename || !*d->filename)
        return false;
    char dir[1024];
    au_cocoa_download_folder(dir, sizeof dir);
    NSString *path = [au_cocoa_str(dir) stringByAppendingPathComponent:au_cocoa_str(d->filename)];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path])
        return false;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
    return true;
}

static void w_prefs_changed(void *ctx, unsigned what, bool committed)
{
    (void)ctx;
    (void)what;
    if (committed && au_cocoa.a)
        au_app_prefs_changed(au_cocoa.a);
    au_cocoa_appearance_apply(); /* the chrome follows the Appearance item */
    for (AuCocoaWin *w in au_cocoa_windows())
        au_cocoa_win_changed(w, AU_CHANGED_LAYOUT);
    au_cocoa_menus_dirty();
    au_cocoa_kick();
}

static uint32_t w_memory_free_kb(void *ctx)
{
    (void)ctx;
    return au_cocoa_memory_free_kb();
}

static AuNet *app_net(void)
{
    return au_cocoa.a ? au_app_net(au_cocoa.a) : NULL;
}

static void w_host_info(void *ctx, AuSysInfo *sys, AuDevHostInfo *h, AuBuf *conn)
{
    (void)ctx;
    au_cocoa_sysinfo(sys);
    AuNet *net = app_net();
    if (net)
        au_net_dev_info(net, h, conn);
}

static void w_connections(void *ctx, AuBuf *out)
{
    (void)ctx;
    AuNet *net = app_net();
    int total, busy;
    if (net)
        au_net_connections_text(net, out, &total, &busy);
}

static void w_record_headers(void *ctx)
{
    (void)ctx;
    AuNet *net = app_net();
    if (net)
        au_net_set_trace_headers(net, true);
}

static void w_windows_changed(void *ctx)
{
    (void)ctx;
    au_cocoa_menus_dirty();
}

static void w_modal_begin(void *ctx)
{
    (void)ctx;
    au_cocoa_modal_begin();
}

static void w_modal_end(void *ctx)
{
    (void)ctx;
    au_cocoa_modal_end();
}

void au_cocoa_windows_start(void)
{
    AuCocoaWindowsHost h;
    memset(&h, 0, sizeof h);
    h.browser = au_cocoa.browser;
    h.fonts = au_cocoa.fonts;
    h.aura_app = au_cocoa.a;
    h.front_window = w_front_window;
    h.front_page = w_front_page;
    h.front_ns_window = w_front_ns_window;
    h.open_url = w_open_url;
    h.reveal_download = w_reveal_download;
    h.prefs_changed = w_prefs_changed;
    h.memory_free_kb = w_memory_free_kb;
    h.host_info = w_host_info;
    h.connections = w_connections;
    /* status_sections stays NULL: System Status shows the network's
     * status (au_app_net_status). No blocking manager, default-browser
     * switch, timing test or paint benchmark yet (docs/cocoa.md, "Known
     * gaps"). */
    h.record_headers = w_record_headers;
    h.windows_changed = w_windows_changed;
    h.modal_begin = w_modal_begin;
    h.modal_end = w_modal_end;
    h.machine = "this Mac";
    h.settings_name = "System Settings";
    h.folder_example = "/Users/you/Downloads";
    h.system_downloads = "Downloads";
    h.no_folder = "your home folder";
    au_cocoa_windows_init(&h);
}
