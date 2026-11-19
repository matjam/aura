/* The AppKit application: start, the tick, quit, session restore and
 * opening URLs (docs/cocoa.md). The order follows src/browser/app.h and
 * the other shells: fonts, the browser, the profile files, the network,
 * then the windows; at quit the reverse.
 *
 * As on every Mac, closing the last window leaves Aura running; Quit (or
 * the Dock's Quit) ends it, and clicking the Dock icon with no window open
 * opens one. */
#import "cocoa.h"
#import "windows_cocoa.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

/* The tick: every 20 ms, or 10 ms while a page, a download or the network
 * is busy. au_cocoa_kick runs a flush sooner (after input, an invalidated
 * rectangle). */
#define TICK_IDLE_MS 20
#define TICK_BUSY_MS 10

AuCocoa au_cocoa;

static NSTimer *tick_timer;
static bool kick_pending;
static NSMutableArray<NSString *> *pending_urls; /* URLs that came before start */
static dispatch_source_t sig_term, sig_int, sig_usr1;

static void flush_all(void)
{
    for (AuCocoaWin *w in [au_cocoa_windows() copy]) {
        if (w->closing)
            au_cocoa_win_destroy(w);
        else
            au_cocoa_win_flush(w);
    }
}

static bool busy(void)
{
    return au_browser_busy(au_cocoa.browser) || au_downloads_busy(&au_cocoa.browser->downloads) ||
           au_app_net_busy(au_cocoa.a);
}

static void schedule_tick(int ms);

static void tick(void)
{
    if (au_cocoa.quitting)
        return;
    /* A dialog the model waits for: nothing may enter the model until it
     * answers. */
    if (!au_cocoa.modal) {
        au_cocoa_net_pump();
        au_downloads_idle(&au_cocoa.browser->downloads, au_posix_ms());
        for (AuCocoaWin *w in au_cocoa_windows())
            if (!w->closing)
                au_bw_idle(w->bw);
        flush_all();
        if (au_cocoa.quitting)
            return;
        au_app_save(au_cocoa.a, false);
    }
    /* Work that could run now (a parked script) runs at the next turn of
     * the run loop, after the events that are waiting. */
    schedule_tick(au_browser_ready(au_cocoa.browser) ? 0 : busy() ? TICK_BUSY_MS : TICK_IDLE_MS);
}

static void schedule_tick(int ms)
{
    [tick_timer invalidate];
    tick_timer = [NSTimer timerWithTimeInterval:ms / 1000.0
                                        repeats:NO
                                          block:^(NSTimer *t) {
                                              (void)t;
                                              tick();
                                          }];
    /* Common modes: the tick runs while a menu is open or a window is
     * resized, as the GTK4 port's does. */
    [[NSRunLoop mainRunLoop] addTimer:tick_timer forMode:NSRunLoopCommonModes];
}

void au_cocoa_kick(void)
{
    if (!au_cocoa.started || au_cocoa.quitting || kick_pending)
        return;
    kick_pending = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        kick_pending = false;
        if (!au_cocoa.modal && !au_cocoa.quitting)
            flush_all();
    });
}

AuCocoaWin *au_cocoa_new_window(const char *url, const AuSessionWindow *sw)
{
    AuPrefs *p = &au_cocoa.browser->prefs;
    int width = p->window_width >= 300 ? p->window_width : 1100;
    int height = p->window_height >= 200 ? p->window_height : 750;
    if (sw && sw->width >= 300 && sw->height >= 200) {
        width = sw->width;
        height = sw->height;
    }
    AuCocoaWin *w = au_cocoa_win_new(width, height);
    if (!w)
        return nil;
    if (sw && sw->ntabs)
        au_bw_restore(w->bw, sw);
    else
        au_bw_new_tab(w->bw, url ? url : p->home_page, true, -1);
    [w->window makeKeyAndOrderFront:nil];
    return w;
}

/* Everything the model holds is saved and freed; then AppKit ends. */
static void teardown(void)
{
    if (au_cocoa.quitting || !au_cocoa.started)
        return;
    au_cocoa.quitting = true;
    [tick_timer invalidate];
    tick_timer = nil;
    /* The secondary windows close first: an open Preferences window is
     * cancelled, so the last save has the preferences from before it. */
    au_cocoa_windows_shutdown();
    /* Privacy options, the last save (the session while the windows are
     * open) and the Running file; then the windows; then the network. */
    au_app_quit(au_cocoa.a);
    while (au_cocoa_windows().count)
        au_cocoa_win_destroy(au_cocoa_windows().firstObject);
    au_cocoa_net_stop();
    au_cocoa_appearance_stop();
    au_app_free(au_cocoa.a);
    au_cocoa.a = NULL;
    au_browser_free(au_cocoa.browser);
    au_cocoa.browser = NULL;
    au_fontset_free(au_cocoa.fonts);
    au_cocoa.fonts = NULL;
    au_cocoa_fonts_free();
}

void au_cocoa_quit(void)
{
    teardown();
    [NSApp terminate:nil];
}

static void open_urls(NSArray<NSString *> *urls)
{
    AuCocoaWin *w = au_cocoa_front();
    for (NSString *u in urls) {
        if (w)
            au_bw_new_tab(w->bw, u.UTF8String, true, -1);
        else
            w = au_cocoa_new_window(u.UTF8String, NULL);
    }
    if (w)
        [w->window makeKeyAndOrderFront:nil];
    au_cocoa_kick();
}

/* Fonts, the browser, the files, the network, the first windows. */
static void start(void)
{
    au_cocoa.fonts = au_fontset_new();
    au_platform_load_fonts(au_cocoa.fonts);
    AuBrowserHost host;
    au_cocoa_browser_host(&host);
    au_cocoa.browser = au_browser_new(au_cocoa.fonts, &host);
    au_cocoa.browser->site_icons = true;
    AuAppPort port;
    bool files = au_cocoa_files_init(&port);
    au_cocoa.a = au_app_new(au_cocoa.browser, &port);
    bool unclean = files && au_app_load(au_cocoa.a);
    au_cocoa_appearance_start();
    if (!au_cocoa_net_start())
        fprintf(stderr, "aura: the network could not be started\n");
    au_cocoa.started = true;
    au_cocoa_windows_start();

    /* The last tabs come back when the preference says so or, after a
     * crash, when the user says so. */
    AuSession session;
    bool restored = false;
    AuAppRestore restore = au_app_session(au_cocoa.a, unclean, &session);
    bool have = restore != AU_APP_RESTORE_NONE;
    if (restore == AU_APP_RESTORE_ASK &&
        !au_cocoa_ask(nil, "Aura did not quit normally last time.", "Reopen the windows and tabs from then?",
                      "Reopen", "Start New")) {
        au_session_free(&session);
        have = false;
    }
    if (have) {
        for (int i = 0; i < session.nwindows; i++)
            if (session.windows[i].ntabs && au_cocoa_new_window(NULL, &session.windows[i]))
                restored = true;
        au_session_free(&session);
    }
    if (pending_urls.count) {
        open_urls(pending_urls);
        restored = true;
    }
    pending_urls = nil;
    if (!restored)
        au_cocoa_new_window(au_app_start_url(au_cocoa.a), NULL);
    schedule_tick(TICK_BUSY_MS);
}

/* A command-line argument: an address with a scheme as typed, else a file
 * or folder path as a file: URL. */
static NSString *arg_url(const char *arg)
{
    NSString *s = au_cocoa_str(arg);
    if ([[NSFileManager defaultManager] fileExistsAtPath:s])
        return [NSURL fileURLWithPath:s].absoluteURL.absoluteString;
    return s;
}

@interface AuAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AuAppDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)n
{
    (void)n;
    au_cocoa_menus_init();
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    (void)n;
    start();
    [NSApp activateIgnoringOtherApps:YES];
}

/* Links from other programs, the Dock and the Finder: tabs of the front
 * window. Before the start they wait for it. */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
    (void)app;
    NSMutableArray<NSString *> *list = [NSMutableArray array];
    for (NSURL *u in urls)
        [list addObject:u.absoluteString];
    if (au_cocoa.quitting)
        return;
    if (!au_cocoa.started) {
        if (!pending_urls)
            pending_urls = [NSMutableArray array];
        [pending_urls addObjectsFromArray:list];
        return;
    }
    if (au_cocoa.modal)
        return;
    open_urls(list);
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible
{
    (void)app;
    if (!visible && au_cocoa.started && !au_cocoa.quitting && !au_cocoa.modal && !au_cocoa_windows().count)
        au_cocoa_new_window(NULL, NULL);
    return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return NO;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    /* While a dialog the model waits for is open, it must answer first. */
    if (au_cocoa.modal && !au_cocoa.quitting)
        return NSTerminateCancel;
    teardown();
    return NSTerminateNow;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
    (void)app;
    return YES;
}

@end

/* For testing without screen recording permission (docs/cocoa.md, "To
 * test"): SIGUSR1 writes the front browser window, its toolbar included,
 * to the PNG file $AURA_SNAPSHOT. The views draw themselves into a bitmap,
 * and the page's pixels, which are a layer's contents, are drawn over the
 * page view's place. */
static void snapshot(void)
{
    const char *path = getenv("AURA_SNAPSHOT");
    AuCocoaWin *w = au_cocoa_front();
    if (!path || !w || w->freed)
        return;
    NSView *frame = w->window.contentView.superview;
    NSBitmapImageRep *rep = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
    [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:rep];
    if (w->pixels && w->pw > 0 && w->ph > 0) {
        NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        CGContextRef cg = gc.CGContext;
        size_t stride = (size_t)w->pw * 4;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, w->pixels, stride * (size_t)w->ph, NULL);
        CGImageRef img = CGImageCreate((size_t)w->pw, (size_t)w->ph, 8, 32, stride, cs,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, NULL, false,
                                       kCGRenderingIntentDefault);
        NSRect r = [w->page convertRect:w->page.bounds toView:frame];
        if (frame.isFlipped)
            r.origin.y = frame.bounds.size.height - NSMaxY(r);
        /* The context already maps points to the bitmap's pixels. */
        CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
        CGContextDrawImage(cg, NSRectToCGRect(r), img);
        CGImageRelease(img);
        CGDataProviderRelease(dp);
        CGColorSpaceRelease(cs);
        [gc flushGraphics];
    }
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:au_cocoa_str(path) atomically:YES];
    /* The toolbar's own layer-backed controls do not draw into the
     * bitmap: list where each item is, and whether it is in the window or
     * in the overflow menu. */
    for (NSToolbarItem *it in w->window.toolbar.items)
        if (it.view)
            fprintf(stderr, "aura snapshot: toolbar %s %s %s\n", it.itemIdentifier.UTF8String,
                    NSStringFromRect(it.view.frame).UTF8String, it.view.window ? "shown" : "in overflow");
        else
            fprintf(stderr, "aura snapshot: toolbar %s button%s\n", it.itemIdentifier.UTF8String,
                    it.enabled ? "" : " (disabled)");
}

static dispatch_source_t on_signal(int sig)
{
    signal(sig, SIG_IGN);
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, dispatch_get_main_queue());
    /* SIGTERM and SIGINT quit as the menu does, so the session is saved. */
    dispatch_source_set_event_handler(s, ^{
        if (sig == SIGUSR1)
            snapshot();
        else
            au_cocoa_quit();
    });
    dispatch_resume(s);
    return s;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        au_cocoa_find_assets();
        pending_urls = [NSMutableArray array];
        for (int i = 1; i < argc; i++) {
            /* -psn_... from old launchers, -NSSomething defaults: not ours. */
            if (argv[i][0] == '-') {
                if (i + 1 < argc && argv[i][1] != 'p' && argv[i + 1][0] != '-')
                    i++; /* a default's value */
                continue;
            }
            [pending_urls addObject:arg_url(argv[i])];
        }
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AuAppDelegate *delegate = [AuAppDelegate new];
        app.delegate = delegate;
        sig_term = on_signal(SIGTERM);
        sig_int = on_signal(SIGINT);
        if (getenv("AURA_SNAPSHOT"))
            sig_usr1 = on_signal(SIGUSR1);
        [app run];
    }
    return 0;
}
