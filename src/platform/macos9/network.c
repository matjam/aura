/* The network stack in the Mac OS 9 application. See mac.h.
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
#include "js/clock.h"

#include "net/cache.h"
#include "net/cookies.h"
#include "net/disk_cache.h"
#include "net/hsts.h"
#include "net/loader.h"
#include "net/net.h"
#include "net/net_prefs.h"
#include "net/roots.h"
#include "net/transfer.h"
#include "net/web_storage.h"

#include <stdio.h>
#include <string.h>

enum { kSaveDelayMs = 5000, kOtRetryMs = 10000 };

static struct {
    AuMacEntropy *entropy;
    AuOtNet *ot;
    bool ot_tried;
    uint32_t ot_tried_ms;
    char offline[200];
    AuNetOps ops; /* the lazy wrapper */
    AuNet *net;
    AuRootStore *roots;
    AuCookieJar *jar;
    AuHsts *hsts;
    AuHttpCache *cache;
    AuDiskCache *disk;
    short cache_vref;
    long cache_dir;
    /* Disk cache file times (microseconds) and counts, for Network Status. */
    uint32_t dc_read_us, dc_write_us;
    unsigned dc_reads, dc_writes;
    AuNetLoader *loader;
    AuNetPageFetcher *pf;
    AuNetTransferSource *xfer;
    bool cookies_pending, hsts_pending;
    uint32_t cookies_first_ms, hsts_first_ms;
    /* Web storage (net/web_storage.h): the Local Storage folder, when
     * its changes began to wait, and file times for System Status. */
    AuWebStorage *storage;
    short ls_vref;
    long ls_dir;
    bool storage_pending;
    uint32_t storage_gen, storage_change_ms, storage_first_ms;
    uint32_t ls_read_us, ls_write_us, ls_write_len;
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

static void load_roots(void)
{
    size_t len = 0;
    /* guest-push.sh would decode a .bin file as MacBinary, so the
     * distribution names it without the extension. */
    char *data = read_beside("aura-roots", &len);
    if (!data)
        data = read_beside("aura-roots.bin", &len);
    if (!data)
        return; /* https then fails with "untrusted issuer" */
    char err[128];
    N.roots = au_roots_load(data, len, err, sizeof err);
    au_free(data);
    if (N.roots)
        au_net_set_roots(N.net, N.roots);
}

/* ---- The disk cache's files: the Cache folder ---- */

/* A file of a folder (the Cache folder, the Local Storage folder). */
static bool folder_spec(short vref, long dir, const char *name, FSSpec *spec)
{
    Str255 p;
    size_t n = strlen(name);
    if (n > 31)
        return false;
    p[0] = (unsigned char)n;
    memcpy(p + 1, name, n);
    OSErr err = FSMakeFSSpec(vref, dir, p, spec);
    return err == noErr || err == fnfErr;
}

static bool cache_spec(const char *name, FSSpec *spec)
{
    return folder_spec(N.cache_vref, N.cache_dir, name, spec);
}

/* Appends the whole file to out. */
static bool folder_read(short vref, long dir, const char *name, AuBuf *out, long max)
{
    FSSpec s;
    short ref;
    if (!folder_spec(vref, dir, name, &s) || FSpOpenDF(&s, fsRdPerm, &ref) != noErr)
        return false;
    long eof = 0;
    bool ok = false;
    if (GetEOF(ref, &eof) == noErr && eof > 0 && eof < max) {
        au_buf_reserve(out, (size_t)eof);
        long count = eof;
        OSErr err = FSRead(ref, &count, out->data + out->len);
        if ((err == noErr || err == eofErr) && count == eof) {
            out->len += (size_t)eof;
            ok = true;
        }
    }
    FSClose(ref);
    return ok;
}

/* Writes "<name>.tmp" and renames it over the file, so a crash in the
 * middle leaves the old file. */
static bool folder_write(short vref, long dir, const char *name, const void *data, size_t len, OSType type)
{
    char tname[40];
    snprintf(tname, sizeof tname, "%s.tmp", name);
    FSSpec tmp, dst;
    if (!folder_spec(vref, dir, tname, &tmp) || !folder_spec(vref, dir, name, &dst))
        return false;
    FSpDelete(&tmp);
    if (FSpCreate(&tmp, 'AURA', type, smSystemScript) != noErr)
        return false;
    short ref;
    if (FSpOpenDF(&tmp, fsRdWrPerm, &ref) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    long count = (long)len;
    OSErr err = FSWrite(ref, &count, data);
    if (err == noErr && count != (long)len)
        err = dskFulErr;
    OSErr cerr = FSClose(ref);
    if (err != noErr || cerr != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    FSpDelete(&dst);
    Str255 p;
    p[0] = (unsigned char)strlen(name);
    memcpy(p + 1, name, p[0]);
    if (FSpRename(&tmp, p) != noErr) {
        FSpDelete(&tmp);
        return false;
    }
    return true;
}

/* ---- Web storage's files: the Local Storage folder ----
 * One file per origin and "Index" (net/web_storage.h). A file that does
 * not parse is renamed "<name> (damaged)". */

static bool ls_read(void *ctx, const char *name, AuBuf *out)
{
    (void)ctx;
    uint32_t t0 = au_mac_clock_us();
    bool ok = folder_read(N.ls_vref, N.ls_dir, name, out, (long)AU_WEB_STORAGE_FILE_MAX + 1);
    N.ls_read_us += au_mac_clock_us() - t0;
    return ok;
}

static bool ls_write(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    uint32_t t0 = au_mac_clock_us();
    bool ok = folder_write(N.ls_vref, N.ls_dir, name, data, len, 'AuLS');
    N.ls_write_us = au_mac_clock_us() - t0; /* the last write, for System Status */
    N.ls_write_len = (uint32_t)len;
    return ok;
}

static void ls_remove(void *ctx, const char *name)
{
    (void)ctx;
    FSSpec s;
    if (folder_spec(N.ls_vref, N.ls_dir, name, &s))
        FSpDelete(&s);
}

static void ls_damaged(void *ctx, const char *name)
{
    (void)ctx;
    char aside[40];
    snprintf(aside, sizeof aside, "%s (damaged)", name);
    FSSpec s, old;
    if (folder_spec(N.ls_vref, N.ls_dir, aside, &old))
        FSpDelete(&old);
    if (folder_spec(N.ls_vref, N.ls_dir, name, &s)) {
        Str255 p;
        p[0] = (unsigned char)strlen(aside);
        memcpy(p + 1, aside, p[0]);
        if (FSpRename(&s, p) != noErr)
            FSpDelete(&s);
    }
}

static void dc_list(void *ctx, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    (void)ctx;
    for (short i = 1; i < 32000; i++) {
        CInfoPBRec pb;
        Str255 name;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = N.cache_vref;
        pb.hFileInfo.ioDirID = N.cache_dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        if (pb.hFileInfo.ioFlAttrib & 0x10)
            continue;
        char cname[32];
        memcpy(cname, name + 1, name[0]);
        cname[name[0]] = 0;
        size_t nl = name[0];
        if (nl > 4 && !strcmp(cname + nl - 4, ".tmp")) {
            /* An interrupted write. */
            FSSpec s;
            if (cache_spec(cname, &s) && FSpDelete(&s) == noErr)
                i--; /* the folder's indices moved down */
            continue;
        }
        each(ec, cname, (size_t)pb.hFileInfo.ioFlLgLen, (uint32_t)pb.hFileInfo.ioFlMdDat);
    }
}

static bool dc_read_file(const char *name, AuBuf *out);
static bool dc_write_file(const char *name, const void *data, size_t len);

static bool dc_read(void *ctx, const char *name, AuBuf *out)
{
    (void)ctx;
    uint32_t t0 = au_mac_clock_us();
    bool ok = dc_read_file(name, out);
    N.dc_read_us += au_mac_clock_us() - t0;
    N.dc_reads++;
    return ok;
}

static bool dc_write(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    uint32_t t0 = au_mac_clock_us();
    bool ok = dc_write_file(name, data, len);
    N.dc_write_us += au_mac_clock_us() - t0;
    N.dc_writes++;
    return ok;
}

static bool dc_read_file(const char *name, AuBuf *out)
{
    return folder_read(N.cache_vref, N.cache_dir, name, out, 32L * 1024 * 1024);
}

static bool dc_write_file(const char *name, const void *data, size_t len)
{
    return folder_write(N.cache_vref, N.cache_dir, name, data, len, 'AuCa');
}

static void dc_remove(void *ctx, const char *name)
{
    (void)ctx;
    FSSpec s;
    if (cache_spec(name, &s))
        FSpDelete(&s);
}

/* ---- Persistence ---- */

static void load_stores(void)
{
    char *data;
    size_t len;
    int64_t now = au_mac_wall_time();
    if (storage_read_named("Cookies", &data, &len)) {
        if (!au_cookie_jar_parse(N.jar, data, len, now))
            storage_keep_aside_named("Cookies");
        au_free(data);
    }
    au_cookie_jar_clear_dirty(N.jar);
    if (storage_read_named("HSTS", &data, &len)) {
        if (!au_hsts_parse_file(N.hsts, data, len, now))
            storage_keep_aside_named("HSTS");
        au_free(data);
    }
    au_hsts_clear_dirty(N.hsts);
    /* Intermediates fetched from caIssuers URLs: never trusted by
     * themselves, so a damaged file only costs a fetch. */
    if (storage_read_named("Intermediates", &data, &len)) {
        au_net_intermediates_parse(N.net, data, len);
        au_free(data);
    }
}

void mac_net_save(bool force)
{
    if (!N.jar)
        return;
    uint32_t now = au_mac_clock_ms();
    if (au_cookie_jar_dirty(N.jar)) {
        if (!N.cookies_pending) {
            N.cookies_pending = true;
            N.cookies_first_ms = now;
        }
        if (force || now - N.cookies_first_ms >= kSaveDelayMs) {
            AuBuf b;
            au_buf_init(&b);
            au_cookie_jar_serialize(N.jar, &b, au_mac_wall_time());
            if (storage_write_named("Cookies", b.data ? b.data : "", b.len)) {
                au_cookie_jar_clear_dirty(N.jar);
                N.cookies_pending = false;
            } else {
                N.cookies_first_ms = now;
            }
            au_buf_free(&b);
        }
    }
    if (au_hsts_dirty(N.hsts)) {
        if (!N.hsts_pending) {
            N.hsts_pending = true;
            N.hsts_first_ms = now;
        }
        if (force || now - N.hsts_first_ms >= kSaveDelayMs) {
            AuBuf b;
            au_buf_init(&b);
            au_hsts_serialize(N.hsts, &b, au_mac_wall_time());
            if (storage_write_named("HSTS", b.data ? b.data : "", b.len)) {
                au_hsts_clear_dirty(N.hsts);
                N.hsts_pending = false;
            } else {
                N.hsts_first_ms = now;
            }
            au_buf_free(&b);
        }
    }
    if (au_net_intermediates_dirty(N.net)) {
        AuBuf b;
        au_buf_init(&b);
        au_net_intermediates_serialize(N.net, &b);
        storage_write_named("Intermediates", b.data, b.len);
        au_buf_free(&b);
    }
    /* Web storage: the changed areas, one file a pass, once script has
     * left them alone for the delay (or half a minute after the first
     * change, for a page that never stops writing), and while nothing
     * loads; all of them when forced. */
    if (N.storage) {
        uint32_t g = au_web_storage_change_gen(N.storage);
        bool pending = au_web_storage_pending(N.storage) > 0;
        if (pending && !N.storage_pending)
            N.storage_first_ms = now;
        N.storage_pending = pending;
        if (g != N.storage_gen) {
            N.storage_gen = g;
            N.storage_change_ms = now;
        }
        if (pending && force)
            au_web_storage_save(N.storage, 0);
        else if (pending && (now - N.storage_change_ms >= kSaveDelayMs || now - N.storage_first_ms >= 30000) &&
                 au_net_loader_active(N.loader) == 0)
            au_web_storage_save(N.storage, 1);
    }
}

/* ---- Life cycle ---- */

static void apply_prefs(void)
{
    const AuPrefs *p = &g_browser->prefs;
    au_net_apply_prefs(N.net, p);
    au_cache_set_max(N.cache, au_net_cache_bytes_from_prefs(p));
    size_t disk = p->disk_cache_kb > 0 ? (size_t)p->disk_cache_kb * 1024 : 0;
    if (N.disk) {
        au_disk_cache_set_max(N.disk, disk);
        if (disk) {
            AuCacheStorage st;
            au_disk_cache_storage(N.disk, &st);
            au_cache_set_storage(N.cache, &st);
            /* The disk has every stored body: memory keeps the last
             * megabyte used instead of the whole memory cache budget. */
            au_cache_set_resident(N.cache, 1024u * 1024u);
        } else {
            au_cache_set_storage(N.cache, NULL);
        }
    }
}

/* Script's Date takes its time and zone from the clock the network stack
 * uses, so Date.now() agrees with cookie expiry and certificate checks.
 * Mac OS 9 has one zone offset and no zone database: every date, past or
 * future, gets the offset set now (daylight saving hour included). The
 * offset is read from parameter RAM at most once a minute. */
static int64_t script_wall_ms(void *ctx)
{
    (void)ctx;
    return au_mac_wall_ms();
}

static int32_t script_utc_offset(void *ctx, int64_t utc_ms)
{
    (void)ctx;
    (void)utc_ms;
    static bool have;
    static uint32_t read_ms;
    static int32_t off;
    uint32_t now = au_mac_clock_ms();
    if (!have || now - read_ms >= 60000u) {
        off = au_mac_gmt_offset();
        read_ms = now;
        have = true;
    }
    return off;
}

void mac_net_init(void)
{
    AuJsClock clock = {NULL, script_wall_ms, script_utc_offset};
    au_js_set_clock(&clock);
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
    N.net = au_net_new(&N.ops);
    load_roots();
    N.jar = au_cookie_jar_new();
    N.hsts = au_hsts_new();
    N.cache = au_cache_new(au_net_cache_bytes_from_prefs(&g_browser->prefs));
    au_net_set_cookie_jar(N.net, N.jar);
    au_net_set_hsts(N.net, N.hsts);
    au_net_set_cache(N.net, N.cache);
    if (storage_subfolder("Cache", &N.cache_vref, &N.cache_dir)) {
        AuDiskCacheFiles files = {NULL, dc_list, dc_read, dc_write, dc_remove};
        N.disk = au_disk_cache_new(&files, 0);
        /* A file write takes about 9 ms here: queue them (mac_net_idle
         * writes them while nothing is loading). */
        au_disk_cache_set_deferred(N.disk, true);
    }
    load_stores();
    /* Web storage follows the cookie rules; its files are in the Local
     * Storage folder, read when an origin is first used. At most 6 MB of
     * it stays in memory. */
    N.storage = au_web_storage_new();
    au_web_storage_set_cookie_jar(N.storage, N.jar);
    au_web_storage_set_limits(N.storage, 0, 0, 6u << 20);
    if (storage_subfolder("Local Storage", &N.ls_vref, &N.ls_dir)) {
        AuWebStorageFiles lsf = {NULL, ls_read, ls_write, ls_remove, ls_damaged};
        au_web_storage_set_files(N.storage, &lsf);
    }
    au_browser_set_storage(g_browser, N.storage);
    apply_prefs();

    N.loader = au_net_loader_new(N.net);
    mac_blocking_init(N.net, N.loader);
    N.pf = au_net_page_fetcher_new(N.loader);
    au_net_page_fetcher_set_entropy(N.pf, entropy_bits, NULL, AU_MAC_ENTROPY_THRESHOLD);
    au_net_page_fetcher_set_offline(N.pf, offline_why, NULL);
    AuPageFetcher f;
    au_net_page_fetcher_interface(N.pf, &f);
    au_browser_set_fetcher(g_browser, "http", &f);
    au_browser_set_fetcher(g_browser, "https", &f);

    AuDownloadFiles files;
    mac_download_files(&files);
    N.xfer = au_net_transfer_source_new(N.loader, N.pf, &files);
    AuTransferSource src;
    au_net_transfer_source_interface(N.xfer, &src);
    g_browser->downloads.src = src;
}

void mac_net_event(const EventRecord *ev)
{
    if (N.entropy)
        au_mac_entropy_event(N.entropy, ev);
}

void mac_net_idle(void)
{
    if (!N.net)
        return;
    au_mac_entropy_idle(N.entropy);
    /* Pages pump the loader from au_page_idle; this covers downloads and
     * connections closing while no page is loading. */
    au_net_loader_poll(N.loader);
    if (N.ot)
        au_ot_net_idle(N.ot);
    /* Queued cache files: one per pass, while no transfer is under way. */
    if (N.disk && au_disk_cache_pending(N.disk) && au_net_loader_active(N.loader) == 0)
        au_disk_cache_idle(N.disk, 1);
    /* Blocklist updates start only while nothing loads. */
    mac_blocking_idle(au_net_loader_active(N.loader) > 0);
}

bool mac_net_busy(void)
{
    /* A load in progress (including one waiting for entropy, which the
     * loop's jitter feeds), or Open Transport finishing a lookup or a
     * connection. */
    return N.net && (au_net_loader_active(N.loader) > 0 || (N.ot && au_ot_net_busy(N.ot)) ||
                     (N.disk && au_disk_cache_pending(N.disk) > 0) || mac_blocking_busy());
}

void mac_net_prefs_changed(void)
{
    if (N.net)
        apply_prefs();
    mac_blocking_prefs_changed();
}

void mac_net_clear_cookies(void)
{
    if (N.jar)
        au_cookie_jar_clear(N.jar);
    /* What sites stored goes with their cookies. */
    if (N.storage)
        au_web_storage_clear(N.storage);
}

AuWebStorage *mac_net_storage(void) { return N.storage; }

size_t mac_net_release_storage(void)
{
    return N.storage ? au_web_storage_release(N.storage) : 0;
}

void mac_net_clear_cache(void)
{
    if (N.cache)
        au_cache_clear(N.cache);
    if (N.disk)
        au_disk_cache_clear(N.disk);
    if (N.net)
        au_net_clear_dns(N.net);
}

AuCookieJar *mac_net_cookie_jar(void) { return N.jar; }

/* The network sections of the System Status report (browser/sysinfo.h),
 * in its layout: a title, then "label value" lines. */
void mac_net_status_text(AuBuf *out)
{
    unsigned seed = 0, startup = 0, jitter = 0, input = 0;
    if (N.entropy)
        au_mac_entropy_sources(N.entropy, &seed, &startup, &jitter, &input);
    au_buf_puts(out, "Network\n");
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
    au_report_kv(out, "Loads under way", "%d", N.loader ? au_net_loader_active(N.loader) : 0);
    au_report_kv(out, "Root certificates", "%u", N.roots ? (unsigned)au_roots_count(N.roots) : 0);
    au_report_kv(out, "Cookies", "%u", N.jar ? (unsigned)au_cookie_jar_count(N.jar) : 0);
    au_report_kv(out, "HSTS hosts", "%u", N.hsts ? (unsigned)au_hsts_count(N.hsts) : 0);
    if (N.storage) {
        /* Counts only: never what a site stored. */
        AuWebStorageStats ws;
        au_web_storage_stats(N.storage, &ws);
        au_report_kv(out, "Web storage",
                     "%u origins, %lu KB   (%u for the session only, %u tab areas, %lu KB in memory, %u writes pending)",
                     ws.local_areas, (unsigned long)(ws.bytes / 1024), ws.session_only_areas, ws.session_areas,
                     (unsigned long)(ws.resident / 1024), ws.pending);
        au_report_kv(out, "Storage files", "%u read in %lu ms, %u written (the last: %lu bytes in %lu ms), %u damaged",
                     ws.reads, (unsigned long)(N.ls_read_us / 1000), ws.writes, (unsigned long)N.ls_write_len,
                     (unsigned long)(N.ls_write_us / 1000), ws.damaged);
    }

    AuCacheStats cs;
    memset(&cs, 0, sizeof cs);
    if (N.cache)
        au_cache_stats(N.cache, &cs);
    au_buf_puts(out, "\nHTTP cache\n");
    au_report_kv(out, "In memory", "%lu KB in %u entries", N.cache ? (unsigned long)(au_cache_bytes(N.cache) / 1024) : 0,
                 N.cache ? (unsigned)au_cache_count(N.cache) : 0);
    au_report_kv(out, "On disk", "%lu KB in %u files", N.disk ? (unsigned long)(au_disk_cache_bytes(N.disk) / 1024) : 0,
                 N.disk ? (unsigned)au_disk_cache_count(N.disk) : 0);
    au_report_kv(out, "Lookups", "%lu fresh, %lu history, %lu revalidated (%lu not modified), %lu missed", cs.fresh,
                 cs.history, cs.stale, cs.not_modified, cs.misses);
    au_report_kv(out, "Read from disk", "%lu entries; %u file reads in %lu ms", cs.from_storage, N.dc_reads,
                 (unsigned long)(N.dc_read_us / 1000));
    au_report_kv(out, "Written to disk", "%u files in %lu ms", N.dc_writes, (unsigned long)(N.dc_write_us / 1000));
    mac_blocking_status_text(out);
}

void mac_net_rules_changed(void)
{
    if (!N.jar)
        return;
    AuBuf b;
    au_buf_init(&b);
    au_cookie_rules_serialize(N.jar, &b);
    au_prefs_set_str(&g_browser->prefs.cookie_sites, au_buf_cstr(&b));
    au_buf_free(&b);
    g_browser->prefs_gen++;
}

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
    if (!N.net)
        return;
    const AuPrefs *p = &g_browser->prefs;
    if (p->clear_cookies_on_quit) {
        au_cookie_jar_clear(N.jar);
        au_web_storage_clear(N.storage);
    }
    au_cookie_jar_end_session(N.jar);
    if (p->clear_cache_on_quit)
        mac_net_clear_cache();
    mac_net_save(true);
    /* Reverse order of creation. The browser's downloads were stopped by
     * the caller (au_downloads_free is not called: the list is saved and
     * the shell exits), so stop them here before the source goes. */
    for (int i = 0; i < g_browser->downloads.n; i++) {
        AuDownload *d = g_browser->downloads.items[i];
        if (d->transfer) {
            g_browser->downloads.src.stop(g_browser->downloads.src.ctx, d->transfer);
            d->transfer = NULL;
        }
    }
    g_browser->downloads.src = *au_sim_source();
    au_net_transfer_source_free(N.xfer);
    au_browser_set_fetcher(g_browser, "http", NULL);
    au_browser_set_fetcher(g_browser, "https", NULL);
    au_net_page_fetcher_free(N.pf);
    au_net_loader_free(N.loader);
    mac_blocking_quit();
    au_net_free(N.net);
    au_cache_free(N.cache);
    au_disk_cache_free(N.disk);
    au_hsts_free(N.hsts);
    au_browser_set_storage(g_browser, NULL); /* saved above (mac_net_save) */
    au_web_storage_free(N.storage);
    au_cookie_jar_free(N.jar);
    au_roots_free(N.roots);
    if (N.ot)
        au_ot_net_free(N.ot);
    au_mac_entropy_free(N.entropy);
    memset(&N, 0, sizeof N);
}

/* ---- Developer windows ---- */

void mac_net_dev_info(struct AuDevHostInfo *h, AuBuf *conn)
{
    if (N.net)
        au_net_dev_info(N.net, h, conn);
}

void mac_net_connections_text(AuBuf *out)
{
    int total, busy;
    if (N.net)
        au_net_connections_text(N.net, out, &total, &busy);
}

void mac_net_trace_headers(bool on)
{
    if (N.net)
        au_net_set_trace_headers(N.net, on);
}
