/* The network stack on the main run loop (docs/porting.md, "The POSIX
 * services", steps 1 to 5; docs/cocoa.md, "The main loop").
 *
 * The descriptors au_posix_net_watch lists are registered with one kqueue,
 * and one dispatch source on the main queue watches that kqueue. A
 * dispatch source per socket would watch descriptors the POSIX layer
 * closes on its own, which libdispatch does not allow; a kqueue forgets a
 * closed descriptor by itself. When the kqueue is ready: drain its events,
 * au_posix_net_ready, then au_net_pump until it reports nothing more, then
 * the loader's poll. The registrations are made again only when
 * au_posix_net_watch_serial changes. The tick in main.m pumps too, for the
 * stack's timeouts. */
#import "cocoa.h"

#include "net/net.h"

#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

static AuPosixNet *posix_net;
static AuDownloadFiles download_files;
static int kq = -1;
static dispatch_source_t kq_source;
static AuPosixWatch watched[AU_POSIX_NET_MAX_WATCH];
static int nwatched;
static unsigned watched_serial;
static bool paused, pumping;

/* Replaces the kqueue's registrations with the descriptors listed now.
 * Deleting a descriptor that was closed fails harmlessly; one closed and
 * reused is deleted and then added again. */
static void rewatch(void)
{
    if (!posix_net || kq < 0)
        return;
    struct kevent ch[2];
    for (int i = 0; i < nwatched; i++) {
        int n = 0;
        if (watched[i].events & AU_POSIX_WATCH_READ)
            EV_SET(&ch[n++], (uintptr_t)watched[i].fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        if (watched[i].events & AU_POSIX_WATCH_WRITE)
            EV_SET(&ch[n++], (uintptr_t)watched[i].fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        for (int k = 0; k < n; k++)
            kevent(kq, &ch[k], 1, NULL, 0, NULL);
    }
    nwatched = au_posix_net_watch(posix_net, watched, AU_POSIX_NET_MAX_WATCH);
    for (int i = 0; i < nwatched; i++) {
        int n = 0;
        /* A read filter also reports errors and hang-ups (EV_EOF). */
        if (watched[i].events & AU_POSIX_WATCH_READ)
            EV_SET(&ch[n++], (uintptr_t)watched[i].fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (watched[i].events & AU_POSIX_WATCH_WRITE)
            EV_SET(&ch[n++], (uintptr_t)watched[i].fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        for (int k = 0; k < n; k++)
            kevent(kq, &ch[k], 1, NULL, 0, NULL);
    }
    watched_serial = au_posix_net_watch_serial(posix_net);
}

void au_cocoa_net_pump(void)
{
    AuNet *net = au_cocoa.a ? au_app_net(au_cocoa.a) : NULL;
    if (!net || paused || pumping)
        return;
    pumping = true;
    au_posix_net_ready(posix_net);
    /* Until nothing more happens, with a bound so the loop stays live. */
    for (int i = 0; i < 64 && au_net_pump(net); i++)
        ;
    au_app_net_idle(au_cocoa.a);
    pumping = false;
    if (au_posix_net_watch_serial(posix_net) != watched_serial)
        rewatch();
}

static void on_ready(void)
{
    /* Empty the kqueue's queue: the filters are level-triggered, so what
     * is still unread after the pump is reported again. */
    struct kevent ev[64];
    struct timespec zero = {0, 0};
    while (kevent(kq, NULL, 0, ev, 64, &zero) == 64)
        ;
    au_cocoa_net_pump();
    au_cocoa_kick();
}

static bool read_roots(void *ctx, char **data, size_t *len)
{
    char path[1200];
    (void)ctx;
    if (!au_cocoa_asset_path("net/aura-roots.bin", path, sizeof path)) {
        fprintf(stderr, "aura: no root certificate store (assets/net/aura-roots.bin): https will fail\n");
        return false;
    }
    return au_platform_read_file(path, data, len);
}

static void net_status(void *ctx, AuBuf *out)
{
    (void)ctx;
    au_buf_printf(out, "Sockets: POSIX, on a kqueue on the main run loop (%d descriptors watched)\n", nwatched);
}

bool au_cocoa_net_start(void)
{
    posix_net = au_posix_net_new();
    if (!posix_net)
        return false;
    kq = kqueue();
    if (kq < 0)
        return false;
    au_cocoa_download_files(&download_files);
    AuAppNet np;
    memset(&np, 0, sizeof np);
    np.ops = au_posix_net_ops(posix_net);
    np.read_roots = read_roots;
    np.downloads = &download_files;
    np.status = net_status;
    if (!au_app_net_start(au_cocoa.a, &np))
        return false;
    kq_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)kq, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(kq_source, ^{
        on_ready();
    });
    dispatch_resume(kq_source);
    rewatch();
    return true;
}

void au_cocoa_net_pause(bool p)
{
    if (p == paused || !kq_source)
        return;
    paused = p;
    if (paused) {
        dispatch_suspend(kq_source);
    } else {
        dispatch_resume(kq_source);
        rewatch();
    }
}

void au_cocoa_net_stop(void)
{
    if (kq_source) {
        if (paused)
            dispatch_resume(kq_source);
        dispatch_source_cancel(kq_source);
        kq_source = nil;
    }
    paused = false;
    au_app_net_stop(au_cocoa.a);
    if (posix_net)
        au_posix_net_free(posix_net);
    posix_net = NULL;
    /* The source is cancelled; the kqueue goes with the process, so its
     * descriptor is never closed under a source still watching it. */
    nwatched = 0;
}
