/* The network stack on the GLib main loop (docs/porting.md, "The POSIX
 * services", steps 1 to 5; docs/gtk.md, "The main loop").
 *
 * Each descriptor au_posix_net_watch lists gets a g_unix_fd_add source
 * for G_IO_IN (and G_IO_OUT when the stack waits to write) plus
 * G_IO_ERR | G_IO_HUP. When one is ready: au_posix_net_ready, then
 * au_net_pump until it reports nothing more, then the loader's poll. The
 * list is made again only when au_posix_net_watch_serial changes. The
 * tick in main.c pumps too, for the stack's timeouts. */
#include "gtk.h"

#include "net/net.h"

#include <glib-unix.h>
#include <stdio.h>
#include <string.h>

static AuPosixNet *posix_net;
static AuDownloadFiles download_files;
static guint sources[AU_POSIX_NET_MAX_WATCH];
static int nsources;
static unsigned watched_serial;
static bool paused, pumping;

static void unwatch(void)
{
    for (int i = 0; i < nsources; i++)
        g_source_remove(sources[i]);
    nsources = 0;
}

static gboolean on_fd(gint fd, GIOCondition cond, gpointer data);

static void rewatch(void)
{
    unwatch();
    if (!posix_net || paused)
        return;
    AuPosixWatch w[AU_POSIX_NET_MAX_WATCH];
    int n = au_posix_net_watch(posix_net, w, AU_POSIX_NET_MAX_WATCH);
    for (int i = 0; i < n; i++) {
        GIOCondition c = G_IO_ERR | G_IO_HUP;
        if (w[i].events & AU_POSIX_WATCH_READ)
            c |= G_IO_IN;
        if (w[i].events & AU_POSIX_WATCH_WRITE)
            c |= G_IO_OUT;
        sources[nsources++] = g_unix_fd_add(w[i].fd, c, on_fd, NULL);
    }
    watched_serial = au_posix_net_watch_serial(posix_net);
}

void au_gtk_net_pump(void)
{
    AuNet *net = au_gtk.a ? au_app_net(au_gtk.a) : NULL;
    if (!net || paused || pumping)
        return;
    pumping = true;
    au_posix_net_ready(posix_net);
    /* Until nothing more happens, with a bound so the loop stays live. */
    for (int i = 0; i < 64 && au_net_pump(net); i++)
        ;
    au_app_net_idle(au_gtk.a);
    pumping = false;
    if (au_posix_net_watch_serial(posix_net) != watched_serial)
        rewatch();
}

static gboolean on_fd(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd;
    (void)cond;
    (void)data;
    au_gtk_net_pump();
    au_gtk_kick();
    /* rewatch may have removed this source; G_SOURCE_CONTINUE on a
     * removed source is harmless. */
    return G_SOURCE_CONTINUE;
}

static bool read_roots(void *ctx, char **data, size_t *len)
{
    char path[1200];
    (void)ctx;
    if (!au_gtk_asset_path("net/aura-roots.bin", path, sizeof path)) {
        fprintf(stderr, "aura: no root certificate store (assets/net/aura-roots.bin): https will fail\n");
        return false;
    }
    return au_platform_read_file(path, data, len);
}

static void net_status(void *ctx, AuBuf *out)
{
    (void)ctx;
    au_buf_printf(out, "Sockets: POSIX, on the GLib main loop (%d descriptors watched)\n", nsources);
}

bool au_gtk_net_start(void)
{
    posix_net = au_posix_net_new();
    if (!posix_net)
        return false;
    au_gtk_download_files(&download_files);
    AuAppNet np;
    memset(&np, 0, sizeof np);
    np.ops = au_posix_net_ops(posix_net);
    np.read_roots = read_roots;
    np.downloads = &download_files;
    np.status = net_status;
    if (!au_app_net_start(au_gtk.a, &np))
        return false;
    rewatch();
    return true;
}

void au_gtk_net_pause(bool p)
{
    if (p == paused)
        return;
    paused = p;
    if (paused)
        unwatch();
    else
        rewatch();
}

void au_gtk_net_stop(void)
{
    unwatch();
    au_app_net_stop(au_gtk.a);
    if (posix_net)
        au_posix_net_free(posix_net);
    posix_net = NULL;
}
