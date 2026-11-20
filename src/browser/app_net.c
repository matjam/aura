/* Application services: the network stack set up from the preferences,
 * its files (Cookies, HSTS, Intermediates, the Cache and Local Storage
 * folders), when they are saved, and its part of System Status. See
 * app.h. */
#include "app_int.h"

#include "netlog.h"
#include "script_hints.h"
#include "script_cache.h"

#include "../net/cookies.h"
#include "../net/hsts.h"
#include "../net/net_prefs.h"

#include <string.h>

/* Largest files read back: a disk cache entry, a web storage area. */
#define AU_APP_CACHE_FILE_MAX (32UL * 1024 * 1024)

/* At most this much web storage stays in memory. */
#define AU_APP_STORAGE_RESIDENT (6UL << 20)

/* ---- Cookies, HSTS and Intermediates ---- */

void au_app_load_stores(const AuAppFiles *f, AuCookieJar *jar, AuHsts *hsts, AuNet *net, int64_t now)
{
    char *data;
    size_t len;
    if (jar) {
        if (au_app_read_file(f, AU_APP_FILE_COOKIES, &data, &len)) {
            if (!au_cookie_jar_parse(jar, data, len, now))
                au_app_set_aside(f, NULL, AU_APP_FILE_COOKIES);
            au_free(data);
        }
        au_cookie_jar_clear_dirty(jar);
    }
    if (hsts) {
        if (au_app_read_file(f, AU_APP_FILE_HSTS, &data, &len)) {
            if (!au_hsts_parse_file(hsts, data, len, now))
                au_app_set_aside(f, NULL, AU_APP_FILE_HSTS);
            au_free(data);
        }
        au_hsts_clear_dirty(hsts);
    }
    /* Intermediates fetched from caIssuers URLs: never trusted by
     * themselves, so a damaged file only costs a fetch. */
    if (net && au_app_read_file(f, AU_APP_FILE_INTERMEDIATES, &data, &len)) {
        au_net_intermediates_parse(net, data, len);
        au_free(data);
    }
}

static bool save_cookies(const AuAppFiles *f, AuCookieJar *jar, int64_t now)
{
    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_serialize(jar, &b, now);
    bool ok = au_app_write_file(f, AU_APP_FILE_COOKIES, &b);
    if (ok)
        au_cookie_jar_clear_dirty(jar);
    au_buf_free(&b);
    return ok;
}

static bool save_hsts(const AuAppFiles *f, AuHsts *hsts, int64_t now)
{
    AuBuf b;
    au_buf_init(&b);
    au_hsts_serialize(hsts, &b, now);
    bool ok = au_app_write_file(f, AU_APP_FILE_HSTS, &b);
    if (ok)
        au_hsts_clear_dirty(hsts);
    au_buf_free(&b);
    return ok;
}

static void save_intermediates(const AuAppFiles *f, AuNet *net)
{
    AuBuf b;
    au_buf_init(&b);
    au_net_intermediates_serialize(net, &b);
    au_app_write_file(f, AU_APP_FILE_INTERMEDIATES, &b);
    au_buf_free(&b);
}

void au_app_save_stores(const AuAppFiles *f, AuCookieJar *jar, AuHsts *hsts, AuNet *net, int64_t now)
{
    if (jar && au_cookie_jar_dirty(jar))
        save_cookies(f, jar, now);
    if (hsts && au_hsts_dirty(hsts))
        save_hsts(f, hsts, now);
    if (net && au_net_intermediates_dirty(net))
        save_intermediates(f, net);
}

void au_app_net_save(AuApp *a, bool force)
{
    if (!a->net_started)
        return;
    const AuAppFiles *f = &a->port.files;
    uint32_t now = au_app_ms(a);
    if (au_cookie_jar_dirty(a->jar)) {
        if (!a->cookies_pending) {
            a->cookies_pending = true;
            a->cookies_first_ms = now;
        }
        if (force || now - a->cookies_first_ms >= AU_APP_NET_SAVE_DELAY_MS) {
            if (save_cookies(f, a->jar, au_app_wall_time(a)))
                a->cookies_pending = false;
            else
                a->cookies_first_ms = now;
        }
    }
    if (au_hsts_dirty(a->hsts)) {
        if (!a->hsts_pending) {
            a->hsts_pending = true;
            a->hsts_first_ms = now;
        }
        if (force || now - a->hsts_first_ms >= AU_APP_NET_SAVE_DELAY_MS) {
            if (save_hsts(f, a->hsts, au_app_wall_time(a)))
                a->hsts_pending = false;
            else
                a->hsts_first_ms = now;
        }
    }
    if (au_net_intermediates_dirty(a->net))
        save_intermediates(f, a->net);
    /* Script hints change with nearly every page, and the file can be a
     * few hundred KB: written at most once a minute, and at quit. */
    if (a->hints && au_script_hints_dirty(a->hints) && (force || now - a->hints_saved_ms >= AU_APP_HINTS_SAVE_MS)) {
        AuBuf hb;
        au_buf_init(&hb);
        au_script_hints_serialize(a->hints, &hb);
        if (au_app_write_file(f, AU_APP_FILE_SCRIPT_HINTS, &hb))
            au_script_hints_clear_dirty(a->hints);
        au_buf_free(&hb);
        a->hints_saved_ms = now;
    }
    /* The script cache's Index, as rarely; its queued scripts when
     * forced (au_app_net_idle writes them otherwise). */
    if (a->scripts) {
        if (force)
            au_script_cache_flush(a->scripts);
        if (au_script_cache_dirty(a->scripts) && (force || now - a->scripts_saved_ms >= AU_APP_HINTS_SAVE_MS)) {
            au_script_cache_save(a->scripts);
            a->scripts_saved_ms = now;
        }
    }
    /* Web storage: the changed areas, one file a pass, once script has
     * left them alone for the delay (or half a minute after the first
     * change, for a page that never stops writing), and while nothing
     * loads; all of them when forced. */
    if (a->storage) {
        uint32_t g = au_web_storage_change_gen(a->storage);
        bool pending = au_web_storage_pending(a->storage) > 0;
        if (pending && !a->storage_pending)
            a->storage_first_ms = now;
        a->storage_pending = pending;
        if (g != a->storage_gen) {
            a->storage_gen = g;
            a->storage_change_ms = now;
        }
        if (pending && force)
            au_web_storage_save(a->storage, 0);
        else if (pending &&
                 (now - a->storage_change_ms >= AU_APP_NET_SAVE_DELAY_MS ||
                  now - a->storage_first_ms >= AU_APP_STORAGE_MAX_WAIT_MS) &&
                 au_net_loader_active(a->loader) == 0)
            au_web_storage_save(a->storage, 1);
    }
}

/* ---- Life cycle ---- */

static void apply_prefs(AuApp *a)
{
    const AuPrefs *p = &a->b->prefs;
    au_net_apply_prefs(a->net, p);
    au_cache_set_max(a->cache, au_net_cache_bytes_from_prefs(p));
    size_t disk = p->disk_cache_kb > 0 ? (size_t)p->disk_cache_kb * 1024 : 0;
    if (a->disk) {
        au_disk_cache_set_max(a->disk, disk);
        if (disk) {
            AuCacheStorage st;
            au_disk_cache_storage(a->disk, &st);
            au_cache_set_storage(a->cache, &st);
            /* The disk has every stored body: memory keeps the last
             * megabyte used instead of the whole memory cache budget. */
            au_cache_set_resident(a->cache, 1024u * 1024u);
        } else {
            au_cache_set_storage(a->cache, NULL);
        }
    }
    /* The script cache may take twice the disk cache's size (the
     * bytecode of Discord's 12.8 MB main script is 15.5 MB), and nothing
     * when the disk cache is off. */
    if (a->scripts)
        au_script_cache_set_max(a->scripts, disk < AU_APP_SCRIPT_CACHE_MAX / 2 ? disk * 2 : AU_APP_SCRIPT_CACHE_MAX);
}

static void load_roots(AuApp *a)
{
    char *data = NULL;
    size_t len = 0;
    if (!a->np.read_roots || !a->np.read_roots(a->np.ctx, &data, &len) || !data)
        return; /* https then fails with "untrusted issuer" */
    char err[128];
    a->roots = au_roots_load(data, len, err, sizeof err);
    au_free(data);
    if (a->roots)
        au_net_set_roots(a->net, a->roots);
}

bool au_app_net_start(AuApp *a, const AuAppNet *np)
{
    if (a->net_started || !np->ops)
        return false;
    AuBrowser *b = a->b;
    a->np = *np;
    a->net = au_net_new(np->ops);
    load_roots(a);
    a->jar = au_cookie_jar_new();
    a->hsts = au_hsts_new();
    a->cache = au_cache_new(au_net_cache_bytes_from_prefs(&b->prefs));
    au_net_set_cookie_jar(a->net, a->jar);
    au_net_set_hsts(a->net, a->hsts);
    au_net_set_cache(a->net, a->cache);
    if (au_app_folder_init(&a->cache_folder, &a->port.files, AU_APP_FOLDER_CACHE, AU_APP_CACHE_FILE_MAX)) {
        a->cache_folder.us = a->port.us;
        a->cache_folder.us_ctx = a->port.ctx;
        AuDiskCacheFiles files;
        au_app_folder_disk_cache(&a->cache_folder, &files);
        a->disk = au_disk_cache_new(&files, 0);
        /* A slow file system queues the writes (au_app_net_idle writes
         * them while nothing is loading). */
        au_disk_cache_set_deferred(a->disk, np->defer_cache_writes);
    }
    au_app_load_stores(&a->port.files, a->jar, a->hsts, a->net, au_app_wall_time(a));
    /* Web storage follows the cookie rules; its files are in the Local
     * Storage folder, read when an origin is first used. */
    a->storage = au_web_storage_new();
    au_web_storage_set_cookie_jar(a->storage, a->jar);
    au_web_storage_set_limits(a->storage, 0, 0, AU_APP_STORAGE_RESIDENT);
    if (au_app_folder_init(&a->ls_folder, &a->port.files, AU_APP_FOLDER_LOCAL_STORAGE,
                           (size_t)AU_WEB_STORAGE_FILE_MAX + 1)) {
        a->ls_folder.us = a->port.us;
        a->ls_folder.us_ctx = a->port.ctx;
        AuWebStorageFiles lsf;
        au_app_folder_web_storage(&a->ls_folder, &lsf);
        au_web_storage_set_files(a->storage, &lsf);
    }
    au_browser_set_storage(b, a->storage);
    /* Script hints: only saves time, so a damaged file is dropped. */
    a->hints = au_script_hints_new();
    {
        char *data;
        size_t len;
        if (au_app_read_file(&a->port.files, AU_APP_FILE_SCRIPT_HINTS, &data, &len)) {
            au_script_hints_parse(a->hints, data, len);
            au_free(data);
        }
        au_script_hints_clear_dirty(a->hints);
        a->hints_saved_ms = au_app_ms(a);
    }
    b->script_hints = au_script_hints_store(a->hints);
    /* The script cache: bytecode of large scripts, in a folder of its
     * own. apply_prefs sets its size. */
    if (au_app_folder_init(&a->scripts_folder, &a->port.files, AU_APP_FOLDER_SCRIPT_CACHE, AU_SCRIPT_CACHE_FILE_MAX)) {
        a->scripts_folder.us = a->port.us;
        a->scripts_folder.us_ctx = a->port.ctx;
        AuDiskCacheFiles files;
        au_app_folder_disk_cache(&a->scripts_folder, &files);
        a->scripts = au_script_cache_new(&files, 0);
        au_script_cache_set_deferred(a->scripts, np->defer_cache_writes);
        a->scripts_saved_ms = au_app_ms(a);
        b->script_code = au_script_cache_store(a->scripts);
    }
    apply_prefs(a);

    a->loader = au_net_loader_new(a->net);
    if (np->started)
        np->started(np->ctx, a->net, a->loader);
    a->pf = au_net_page_fetcher_new(a->loader);
    if (np->entropy_bits)
        au_net_page_fetcher_set_entropy(a->pf, np->entropy_bits, np->ctx, np->entropy_threshold);
    if (np->offline_why)
        au_net_page_fetcher_set_offline(a->pf, np->offline_why, np->ctx);
    AuPageFetcher f;
    au_net_page_fetcher_interface(a->pf, &f);
    au_browser_set_fetcher(b, "http", &f);
    au_browser_set_fetcher(b, "https", &f);

    if (np->downloads) {
        a->xfer = au_net_transfer_source_new(a->loader, a->pf, np->downloads);
        AuTransferSource src;
        au_net_transfer_source_interface(a->xfer, &src);
        b->downloads.src = src;
    }
    a->net_started = true;
    return true;
}

void au_app_net_idle(AuApp *a)
{
    if (!a->net_started)
        return;
    /* Pages pump the loader from au_page_idle; this covers downloads and
     * connections closing while no page is loading. */
    au_net_loader_poll(a->loader);
    /* Queued cache files: one per pass, while no transfer is under way. */
    if (a->disk && au_disk_cache_pending(a->disk) && au_net_loader_active(a->loader) == 0)
        au_disk_cache_idle(a->disk, 1);
    else if (a->scripts && au_script_cache_pending(a->scripts) && au_net_loader_active(a->loader) == 0)
        au_script_cache_idle(a->scripts, 1);
}

int au_app_loads_active(AuApp *a)
{
    return a->net_started ? au_net_loader_active(a->loader) : 0;
}

bool au_app_net_busy(AuApp *a)
{
    return a->net_started &&
           (au_net_loader_active(a->loader) > 0 || (a->disk && au_disk_cache_pending(a->disk) > 0) ||
            (a->scripts && au_script_cache_pending(a->scripts) > 0));
}

void au_app_prefs_changed(AuApp *a)
{
    if (a->net_started)
        apply_prefs(a);
}

void au_app_clear_cookies(AuApp *a)
{
    if (a->jar)
        au_cookie_jar_clear(a->jar);
    /* What sites stored goes with their cookies. */
    if (a->storage)
        au_web_storage_clear(a->storage);
}

void au_app_clear_cache(AuApp *a)
{
    if (a->cache)
        au_cache_clear(a->cache);
    if (a->disk)
        au_disk_cache_clear(a->disk);
    if (a->net)
        au_net_clear_dns(a->net);
    if (a->hints)
        au_script_hints_clear(a->hints); /* it names the scripts pages ran */
    if (a->scripts)
        au_script_cache_clear(a->scripts); /* so do its files */
}

void au_app_cookie_rules_changed(AuApp *a)
{
    if (!a->jar)
        return;
    AuBuf b;
    au_buf_init(&b);
    au_cookie_rules_serialize(a->jar, &b);
    au_prefs_set_str(&a->b->prefs.cookie_sites, au_buf_cstr(&b));
    au_buf_free(&b);
    a->b->prefs_gen++;
}

size_t au_app_release_storage(AuApp *a)
{
    return a->storage ? au_web_storage_release(a->storage) : 0;
}

AuNet *au_app_net(AuApp *a) { return a->net; }
AuNetLoader *au_app_loader(AuApp *a) { return a->loader; }
AuCookieJar *au_app_cookie_jar(AuApp *a) { return a->jar; }
AuWebStorage *au_app_storage(AuApp *a) { return a->storage; }

void au_app_net_status(AuApp *a, AuBuf *out)
{
    au_buf_puts(out, "Network\n");
    if (a->np.status)
        a->np.status(a->np.ctx, out);
    au_report_kv(out, "Loads under way", "%d", au_app_loads_active(a));
    au_report_kv(out, "Root certificates", "%u", a->roots ? (unsigned)au_roots_count(a->roots) : 0);
    au_report_kv(out, "Cookies", "%u", a->jar ? (unsigned)au_cookie_jar_count(a->jar) : 0);
    au_report_kv(out, "HSTS hosts", "%u", a->hsts ? (unsigned)au_hsts_count(a->hsts) : 0);
    if (a->storage) {
        /* Counts only: never what a site stored. */
        const AuAppFolder *ls = &a->ls_folder;
        AuWebStorageStats ws;
        au_web_storage_stats(a->storage, &ws);
        au_report_kv(out, "Web storage", "%u origins, %lu KB, %u writes pending", ws.local_areas + ws.session_only_areas,
                     (unsigned long)(ws.bytes / 1024), ws.pending);
        au_report_kv(out, "Storage areas", "%u session only, %u of tabs, %lu KB in memory", ws.session_only_areas,
                     ws.session_areas, (unsigned long)(ws.resident / 1024));
        au_report_kv(out, "Storage files", "%u read in %lu ms, %u written, %u damaged", ws.reads,
                     (unsigned long)(ls->read_us / 1000), ws.writes, ws.damaged);
        au_report_kv(out, "Largest write", "%lu bytes in %lu ms", (unsigned long)ls->largest_len,
                     (unsigned long)(ls->largest_us / 1000));
    }

    const AuAppFolder *dc = &a->cache_folder;
    AuCacheStats cs;
    memset(&cs, 0, sizeof cs);
    if (a->cache)
        au_cache_stats(a->cache, &cs);
    au_buf_puts(out, "\nHTTP cache\n");
    au_report_kv(out, "In memory", "%lu KB in %u entries", a->cache ? (unsigned long)(au_cache_bytes(a->cache) / 1024) : 0,
                 a->cache ? (unsigned)au_cache_count(a->cache) : 0);
    au_report_kv(out, "On disk", "%lu KB in %u files", a->disk ? (unsigned long)(au_disk_cache_bytes(a->disk) / 1024) : 0,
                 a->disk ? (unsigned)au_disk_cache_count(a->disk) : 0);
    au_report_kv(out, "Lookups", "%lu fresh, %lu history, %lu revalidated (%lu not modified), %lu missed", cs.fresh,
                 cs.history, cs.stale, cs.not_modified, cs.misses);
    au_report_kv(out, "Read from disk", "%lu entries; %u file reads in %lu ms", cs.from_storage, dc->reads,
                 (unsigned long)(dc->read_us / 1000));
    au_report_kv(out, "Written to disk", "%u files in %lu ms", dc->writes, (unsigned long)(dc->write_us / 1000));

    if (a->scripts) {
        /* Counts only: never which scripts. */
        const AuAppFolder *sf = &a->scripts_folder;
        AuScriptCacheStats ss;
        au_script_cache_stats(a->scripts, &ss);
        au_buf_puts(out, "\nScript cache\n");
        au_report_kv(out, "On disk", "%lu KB in %u scripts, %u seen once", (unsigned long)(ss.bytes / 1024),
                     (unsigned)ss.count, (unsigned)ss.seen);
        au_report_kv(out, "Lookups", "%u read, %u missed, %u stored, %u damaged", ss.hits, ss.misses, ss.stores,
                     ss.damaged);
        au_report_kv(out, "Files", "%u read in %lu ms, %u written in %lu ms", sf->reads,
                     (unsigned long)(sf->read_us / 1000), sf->writes, (unsigned long)(sf->write_us / 1000));
    }
}

void au_app_net_stop(AuApp *a)
{
    if (!a->net_started)
        return;
    AuBrowser *b = a->b;
    const AuPrefs *p = &b->prefs;
    if (p->clear_cookies_on_quit) {
        au_cookie_jar_clear(a->jar);
        au_web_storage_clear(a->storage);
    }
    au_cookie_jar_end_session(a->jar);
    if (p->clear_cache_on_quit)
        au_app_clear_cache(a);
    au_app_net_save(a, true);
    /* Reverse order of creation. The browser's downloads list is saved
     * and kept (au_downloads_free is not called), so stop its transfers
     * here before the source goes. */
    if (a->xfer) {
        for (int i = 0; i < b->downloads.n; i++) {
            AuDownload *d = b->downloads.items[i];
            if (d->transfer) {
                b->downloads.src.stop(b->downloads.src.ctx, d->transfer);
                d->transfer = NULL;
            }
        }
        b->downloads.src = *au_sim_source();
        au_net_transfer_source_free(a->xfer);
    }
    au_browser_set_fetcher(b, "http", NULL);
    au_browser_set_fetcher(b, "https", NULL);
    au_net_page_fetcher_free(a->pf);
    au_net_loader_free(a->loader);
    if (a->np.stopping)
        a->np.stopping(a->np.ctx);
    au_net_free(a->net);
    au_cache_free(a->cache);
    au_disk_cache_free(a->disk);
    au_hsts_free(a->hsts);
    au_browser_set_storage(b, NULL); /* saved above */
    au_web_storage_free(a->storage);
    b->script_hints = NULL;
    au_script_hints_free(a->hints);
    a->hints = NULL;
    b->script_code = NULL;
    au_script_cache_free(a->scripts);
    a->scripts = NULL;
    au_cookie_jar_free(a->jar);
    au_roots_free(a->roots);
    a->net = NULL;
    a->roots = NULL;
    a->jar = NULL;
    a->hsts = NULL;
    a->cache = NULL;
    a->disk = NULL;
    a->loader = NULL;
    a->pf = NULL;
    a->xfer = NULL;
    a->storage = NULL;
    a->net_started = false;
}
