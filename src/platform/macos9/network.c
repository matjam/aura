/* The network stack in the Mac OS 9 application. See mac.h.
 *
 * The stack itself (the AuNet, its stores and caches, web storage, the
 * loader and page fetcher, downloads, their files and when they are
 * saved) is set up by src/browser/app_net.c. This file gives it what is
 * Mac OS 9's own: Open Transport, the entropy pool, the root store beside
 * the application, download files and content blocking.
 *
 * Open Transport starts on the first request that needs it (a DNS
 * lookup), not at launch: a session that only shows local pages never
 * loads the TCP/IP stack, and a machine without Open Transport or with
 * TCP/IP unconfigured gets an error page instead of a failed launch.
 * The AuNetOps the engine sees is a thin wrapper that starts Open
 * Transport then and forwards to net_ot.c; when the start fails, the
 * page fetcher's offline hook turns the failure into the "No network"
 * page with the reason. A failed start is tried again after 10 seconds,
 * for the user who fixes the TCP/IP control panel and reloads.
 *
 * The entropy pool starts at launch, so it gathers from the user's first
 * events. Everything runs on the main task; the loop calls mac_net_event
 * for each event and mac_net_idle once per pass. */
#include "mac.h"

#include "blocking_mac.h"
#include "clock_mac.h"
#include "entropy.h"
#include "net_ot.h"

#include "browser/netlog.h"

#include "net/loader.h"
#include "net/net.h"
#include "net/transfer.h"

#include <stdio.h>
#include <string.h>

enum { kOtRetryMs = 10000 };

static struct {
    AuMacEntropy *entropy;
    AuOtNet *ot;
    bool ot_tried;
    uint32_t ot_tried_ms;
    char offline[200];
    AuNetOps ops; /* the lazy wrapper */
    AuDownloadFiles downloads;
    bool started;
} N;

/* ---- Open Transport, started on demand ---- */

static bool ensure_ot(void)
{
    if (N.ot)
        return true;
    uint32_t now = au_mac_clock_ms();
    if (N.ot_tried && now - N.ot_tried_ms < kOtRetryMs)
        return false;
    N.ot_tried = true;
    N.ot_tried_ms = now;
    long err = 0;
    N.ot = au_ot_net_new(N.entropy, &err);
    if (N.ot) {
        N.offline[0] = 0;
        return true;
    }
    if (err == 1)
        snprintf(N.offline, sizeof N.offline,
                 "Open Transport and its TCP/IP stack are not installed on this Macintosh, so Aura cannot reach "
                 "the network.");
    else
        snprintf(N.offline, sizeof N.offline,
                 "Open Transport would not start (error %ld). Check the TCP/IP control panel, then reload the "
                 "page.",
                 err);
    return false;
}

static const AuNetOps *ot_ops(void) { return N.ot ? au_ot_net_ops(N.ot) : NULL; }

static uint32_t w_resolve_start(void *ctx, const char *host)
{
    (void)ctx;
    if (!ensure_ot())
        return 0;
    const AuNetOps *o = ot_ops();
    return o->resolve_start(o->ctx, host);
}

static int w_resolve_poll(void *ctx, uint32_t req, AuNetAddr *addr)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    return o ? o->resolve_poll(o->ctx, req, addr) : -1;
}

static void w_resolve_cancel(void *ctx, uint32_t req)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    if (o)
        o->resolve_cancel(o->ctx, req);
}

static uint32_t w_connect(void *ctx, const AuNetAddr *addr, uint16_t port)
{
    (void)ctx;
    if (!ensure_ot())
        return 0;
    const AuNetOps *o = ot_ops();
    return o->connect(o->ctx, addr, port);
}

static int w_connect_poll(void *ctx, uint32_t sock)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    return o ? o->connect_poll(o->ctx, sock) : -1;
}

static long w_send(void *ctx, uint32_t sock, const void *data, size_t len)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    return o ? o->send(o->ctx, sock, data, len) : AU_NETIO_ERROR;
}

static long w_recv(void *ctx, uint32_t sock, void *buf, size_t cap)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    return o ? o->recv(o->ctx, sock, buf, cap) : AU_NETIO_ERROR;
}

static void w_close(void *ctx, uint32_t sock)
{
    (void)ctx;
    const AuNetOps *o = ot_ops();
    if (o)
        o->close(o->ctx, sock);
}

static uint32_t w_now_ms(void *ctx)
{
    (void)ctx;
    return au_mac_clock_ms();
}

static int64_t w_wall_time(void *ctx)
{
    (void)ctx;
    return au_mac_wall_time();
}

static bool w_entropy(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    return au_mac_entropy_read(N.entropy, buf, len);
}

bool mac_net_random(void *buf, size_t len)
{
    return N.entropy && au_mac_entropy_read(N.entropy, buf, len);
}

static unsigned entropy_bits(void *ctx)
{
    (void)ctx;
    return au_mac_entropy_bits(N.entropy);
}

static const char *offline_why(void *ctx)
{
    (void)ctx;
    return N.offline[0] ? N.offline : NULL;
}

/* ---- The root store beside the application ---- */

static bool app_dir(short *vref, long *dir)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec spec;
    Str255 name;
    if (GetCurrentProcess(&psn) != noErr)
        return false;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processName = name;
    info.processAppSpec = &spec;
    if (GetProcessInformation(&psn, &info) != noErr)
        return false;
    *vref = spec.vRefNum;
    *dir = spec.parID;
    return true;
}

static char *read_beside(const char *fname, size_t *len)
{
    short vref;
    long dir;
    if (!app_dir(&vref, &dir))
        return NULL;
    Str255 p;
    size_t n = strlen(fname);
    p[0] = (unsigned char)n;
    memcpy(p + 1, fname, n);
    FSSpec spec;
    if (FSMakeFSSpec(vref, dir, p, &spec) != noErr)
        return NULL;
    short ref;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return NULL;
    long eof = 0;
    char *buf = NULL;
    if (GetEOF(ref, &eof) == noErr && eof > 0 && eof < 4L * 1024 * 1024) {
        buf = au_alloc((size_t)eof + 1);
        long count = eof;
        OSErr err = FSRead(ref, &count, buf);
        if ((err == noErr || err == eofErr) && count == eof) {
            buf[eof] = 0;
            *len = (size_t)eof;
        } else {
            au_free(buf);
            buf = NULL;
        }
    }
    FSClose(ref);
    return buf;
}

static bool read_roots(void *ctx, char **data, size_t *len)
{
    (void)ctx;
    /* guest-push.sh would decode a .bin file as MacBinary, so the
     * distribution names it without the extension. */
    *data = read_beside("aura-roots", len);
    if (!*data)
        *data = read_beside("aura-roots.bin", len);
    return *data != NULL;
}

/* ---- Content blocking, attached to the loader ---- */

static void net_started(void *ctx, AuNet *net, AuNetLoader *loader)
{
    (void)ctx;
    mac_blocking_init(net, loader);
}

static void net_stopping(void *ctx)
{
    (void)ctx;
    mac_blocking_quit();
}

/* The entropy and Open Transport lines of System Status. */
static void net_status(void *ctx, AuBuf *out)
{
    (void)ctx;
    unsigned seed = 0, startup = 0, jitter = 0, input = 0;
    if (N.entropy)
        au_mac_entropy_sources(N.entropy, &seed, &startup, &jitter, &input);
    au_report_kv(out, "Entropy", "%u of %u bits   (seed file %u, start-up %u, timing jitter %u, input %u)",
                 N.entropy ? au_mac_entropy_bits(N.entropy) : 0, (unsigned)AU_MAC_ENTROPY_THRESHOLD, seed, startup, jitter,
                 input);
    if (N.ot) {
        AuOtNetStats st;
        au_ot_net_stats(N.ot, &st);
        au_report_kv(out, "Open Transport", "started");
        au_report_kv(out, "Name lookups", "%u   (%u failed)", st.lookups, st.lookups_failed);
        au_report_kv(out, "Connections made", "%u   (%u failed)", st.connects, st.connects_failed);
        au_report_kv(out, "Data", "%lu KB sent, %lu KB received", st.bytes_sent / 1024, st.bytes_received / 1024);
        au_report_kv(out, "Last error", "%ld", st.last_error);
    } else {
        au_report_kv(out, "Open Transport", "%s", N.offline[0] ? N.offline : "not started (no request yet)");
    }
}

/* ---- Life cycle ---- */

void mac_net_init(void)
{
    N.entropy = au_mac_entropy_new();
    N.ops.ctx = NULL;
    N.ops.resolve_start = w_resolve_start;
    N.ops.resolve_poll = w_resolve_poll;
    N.ops.resolve_cancel = w_resolve_cancel;
    N.ops.connect = w_connect;
    N.ops.connect_poll = w_connect_poll;
    N.ops.send = w_send;
    N.ops.recv = w_recv;
    N.ops.close = w_close;
    N.ops.now_ms = w_now_ms;
    N.ops.wall_time = w_wall_time;
    N.ops.entropy = w_entropy;
    mac_download_files(&N.downloads);

    AuAppNet np;
    memset(&np, 0, sizeof np);
    np.ops = &N.ops;
    np.entropy_bits = entropy_bits;
    np.entropy_threshold = AU_MAC_ENTROPY_THRESHOLD;
    np.offline_why = offline_why;
    np.read_roots = read_roots;
    np.downloads = &N.downloads;
    /* A file write takes about 9 ms here: queue them (mac_net_idle
     * writes them while nothing is loading). */
    np.defer_cache_writes = true;
    np.started = net_started;
    np.stopping = net_stopping;
    np.status = net_status;
    N.started = au_app_net_start(g_app, &np);
}

void mac_net_event(const EventRecord *ev)
{
    if (N.entropy)
        au_mac_entropy_event(N.entropy, ev);
}

void mac_net_idle(void)
{
    if (!N.started)
        return;
    au_mac_entropy_idle(N.entropy);
    au_app_net_idle(g_app);
    if (N.ot)
        au_ot_net_idle(N.ot);
    /* Blocklist updates start only while nothing loads. */
    mac_blocking_idle(au_app_loads_active(g_app) > 0);
}

bool mac_net_busy(void)
{
    /* A load in progress (including one waiting for entropy, which the
     * loop's jitter feeds), queued cache files, Open Transport finishing
     * a lookup or a connection, or a blocklist being updated. */
    return N.started && (au_app_net_busy(g_app) || (N.ot && au_ot_net_busy(N.ot)) || mac_blocking_busy());
}

void mac_net_prefs_changed(void)
{
    au_app_prefs_changed(g_app);
    mac_blocking_prefs_changed();
}

void mac_net_clear_cookies(void) { au_app_clear_cookies(g_app); }

struct AuWebStorage *mac_net_storage(void) { return g_app ? au_app_storage(g_app) : NULL; }

size_t mac_net_release_storage(void) { return g_app ? au_app_release_storage(g_app) : 0; }

void mac_net_clear_cache(void) { au_app_clear_cache(g_app); }

AuCookieJar *mac_net_cookie_jar(void) { return g_app ? au_app_cookie_jar(g_app) : NULL; }

/* The network sections of the System Status report (browser/sysinfo.h),
 * in its layout: a title, then "label value" lines. */
void mac_net_status_text(AuBuf *out)
{
    au_app_net_status(g_app, out);
    mac_blocking_status_text(out);
}

void mac_net_rules_changed(void) { au_app_cookie_rules_changed(g_app); }

/* Open Transport's notifiers point into Aura's heap, and with Apple's
 * glue absent nothing closes Aura's client at ExitToShell: an exit with
 * providers open leaves the system calling into freed memory (the guest
 * froze after the out-of-memory alert). Closing allocates nothing. */
void mac_net_emergency_close(void)
{
    if (N.ot) {
        au_ot_net_free(N.ot);
        N.ot = NULL;
        N.ot_tried = true;
        N.ot_tried_ms = au_mac_clock_ms();
        snprintf(N.offline, sizeof N.offline, "Open Transport was closed.");
    }
}

void mac_net_quit(void)
{
    if (!N.started)
        return;
    /* The privacy options, the last save, then the stack in reverse order
     * (content blocking goes from the stopping hook, before the AuNet);
     * then Open Transport, and the entropy pool writes its seed file. */
    au_app_net_stop(g_app);
    if (N.ot)
        au_ot_net_free(N.ot);
    au_mac_entropy_free(N.entropy);
    memset(&N, 0, sizeof N);
}

/* ---- Developer windows ---- */

void mac_net_dev_info(struct AuDevHostInfo *h, AuBuf *conn)
{
    AuNet *net = g_app ? au_app_net(g_app) : NULL;
    if (net)
        au_net_dev_info(net, h, conn);
}

void mac_net_connections_text(AuBuf *out)
{
    int total, busy;
    AuNet *net = g_app ? au_app_net(g_app) : NULL;
    if (net)
        au_net_connections_text(net, out, &total, &busy);
}

void mac_net_trace_headers(bool on)
{
    AuNet *net = g_app ? au_app_net(g_app) : NULL;
    if (net)
        au_net_set_trace_headers(net, on);
}
