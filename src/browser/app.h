/* Application services: the files Aura keeps, loading them at start,
 * saving them as they change, the unclean exit marker, session restore,
 * and the network stack set up from the preferences.
 *
 * Every port needs the same decisions here, so they live in the shared
 * code. A port gives the folder primitives (AuAppFiles), its clocks
 * (AuAppPort) and, to start the network, its AuNetOps and a few hooks
 * (AuAppNet). See docs/porting.md and docs/browser-chrome.md ("Files
 * Aura keeps").
 *
 * The files, in the port's profile folder (on Mac OS 9 the folder "Aura"
 * in the system Preferences folder):
 *     Aura Preferences   key=value preferences (prefs.h)
 *     Bookmarks.html     Netscape bookmark file (bookmarks.h)
 *     History            history log (history.h)
 *     Session            open windows and tabs (browser.h)
 *     Downloads          the downloads list (downloads.h)
 *     Favicons           site icons by host (favicon.h)
 *     Running            exists while Aura runs (unclean exit detection)
 *     Cookies            persistent cookies (net/cookies.h)
 *     HSTS               Strict-Transport-Security hosts (net/hsts.h)
 *     Intermediates      fetched intermediate certificates (net/net.h)
 *     Cache              folder of disk cache entries (net/disk_cache.h)
 *     Local Storage      folder of web storage areas (net/web_storage.h)
 * The port itself keeps "Random Seed" (its entropy pool, when it has
 * one) and "Blocklists" (content blocking) in the same folder.
 *
 * A file that fails to parse is renamed "<name> (damaged)" before
 * anything overwrites it, and the defaults are used. A data set is
 * written a few seconds after its first unsaved change and at once on
 * quit; each set's change counter is compared with the one last saved,
 * so only changed sets are written.
 *
 * Use, in order:
 *     a = au_app_new(browser, &port);
 *     unclean = au_app_load(a);          (when the profile folder exists)
 *     au_app_net_start(a, &net);
 *     au_app_session(a, &session) ...    (restore windows, or open one)
 *     loop: au_app_net_idle(a); ... au_app_save(a, false);
 *     au_app_quit(a);                    (before the windows close)
 *     au_app_net_stop(a);                (after the pages are gone)
 *     au_app_free(a);
 */
#ifndef AURA_BROWSER_APP_H
#define AURA_BROWSER_APP_H

#include "browser.h"
#include "../net/net.h"

#define AU_APP_FILE_PREFS "Aura Preferences"
#define AU_APP_FILE_BOOKMARKS "Bookmarks.html"
#define AU_APP_FILE_HISTORY "History"
#define AU_APP_FILE_SESSION "Session"
#define AU_APP_FILE_DOWNLOADS "Downloads"
#define AU_APP_FILE_FAVICONS "Favicons"
#define AU_APP_FILE_RUNNING "Running"
#define AU_APP_FILE_COOKIES "Cookies"
#define AU_APP_FILE_HSTS "HSTS"
#define AU_APP_FILE_INTERMEDIATES "Intermediates"
#define AU_APP_FILE_SCRIPT_HINTS "Script Hints"
#define AU_APP_FILE_RANDOM_SEED "Random Seed"
#define AU_APP_FOLDER_CACHE "Cache"
#define AU_APP_FOLDER_LOCAL_STORAGE "Local Storage"
#define AU_APP_FOLDER_BLOCKLISTS "Blocklists"

/* Delays, in milliseconds. */
enum {
    AU_APP_SAVE_DELAY_MS = 3000,        /* preferences, bookmarks, history, session, downloads */
    AU_APP_SCROLL_SAVE_DELAY_MS = 30000, /* the session while only scroll positions changed; site icons */
    AU_APP_NET_SAVE_DELAY_MS = 5000,    /* cookies, HSTS, web storage after its last change */
    AU_APP_STORAGE_MAX_WAIT_MS = 30000, /* web storage after its first unsaved change */
    AU_APP_HINTS_SAVE_MS = 60000        /* script hints, at most this often */
};

/* The port's files. folder is NULL for the profile folder itself, else
 * the name of a folder inside it (AU_APP_FOLDER_*). Names are at most
 * 31 bytes. */
typedef struct AuAppFiles {
    void *ctx;
    /* Appends the whole file to out. False when it is missing, cannot be
     * read, or is max bytes or larger. */
    bool (*read)(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max);
    /* Writes the file safely: a temporary file, then replaced, so a crash
     * in the middle leaves the old file (Mac OS 9: FSpExchangeFiles). */
    bool (*write)(void *ctx, const char *folder, const char *name, const void *data, size_t len);
    /* Renames name to new_name, replacing a file new_name. */
    bool (*rename)(void *ctx, const char *folder, const char *name, const char *new_name);
    void (*remove)(void *ctx, const char *folder, const char *name);
    bool (*exists)(void *ctx, const char *folder, const char *name);
    /* Makes the folder when it is missing; false when it cannot be used. */
    bool (*make_folder)(void *ctx, const char *folder);
    /* Calls each for every file of the folder (mtime in any unit that
     * orders by age). The port leaves out its own temporary files. */
    void (*list)(void *ctx, const char *folder, void (*each)(void *each_ctx, const char *name, size_t size, uint32_t mtime),
                 void *each_ctx);
} AuAppFiles;

typedef struct AuAppPort {
    AuAppFiles files;
    void *ctx; /* for the clocks */
    uint32_t (*ms)(void *ctx);        /* monotonic milliseconds */
    int64_t (*wall_time)(void *ctx);  /* seconds since 1970 UTC */
    int32_t (*utc_offset)(void *ctx); /* seconds east of UTC now */
    /* Optional. UTC milliseconds for script's Date: when set, au_app_new
     * installs the script clock (js/clock.h) with this and utc_offset,
     * read at most once a minute. */
    int64_t (*wall_ms)(void *ctx);
    /* Optional. Microseconds for timing file work (System Status). */
    uint32_t (*us)(void *ctx);
} AuAppPort;

/* What the network stack needs from the port. Every hook is optional. */
struct AuDownloadFiles;
struct AuNetLoader;
typedef struct AuAppNet {
    const AuNetOps *ops; /* kept, not copied */
    void *ctx;
    /* The page fetcher waits for entropy_threshold bits from the pool. */
    unsigned (*entropy_bits)(void *ctx);
    unsigned entropy_threshold;
    /* Why the network cannot be used now (the No network page), or NULL. */
    const char *(*offline_why)(void *ctx);
    /* The root store's bytes (au_alloc'd). */
    bool (*read_roots)(void *ctx, char **data, size_t *len);
    /* Download files; NULL keeps the browser's simulated source. */
    const struct AuDownloadFiles *downloads;
    /* Queue disk cache writes for au_app_net_idle (slow file systems). */
    bool defer_cache_writes;
    /* After the loader is made (content blocking attaches here), and
     * after it is freed but before the AuNet is. */
    void (*started)(void *ctx, AuNet *net, struct AuNetLoader *loader);
    void (*stopping)(void *ctx);
    /* The port's lines of the System Status "Network" section (entropy,
     * the socket layer), written after its title. */
    void (*status)(void *ctx, AuBuf *out);
} AuAppNet;

typedef struct AuApp AuApp;

AuApp *au_app_new(AuBrowser *b, const AuAppPort *port);
void au_app_free(AuApp *a); /* after au_app_net_stop */

/* Loads the files (preferences first; first-run bookmarks when there is
 * no bookmark file) and marks Aura as running. Returns true when the last
 * run did not quit normally (its Running file was still there). Call it
 * only when the profile folder could be found or made. */
bool au_app_load(AuApp *a);

/* Whether to reopen the last session, and the session (free it with
 * au_session_free unless the result is AU_APP_RESTORE_NONE). ASK: the
 * last run ended uncleanly and the preferences do not restore anyway;
 * the port asks the user. */
typedef enum { AU_APP_RESTORE_NONE, AU_APP_RESTORE_YES, AU_APP_RESTORE_ASK } AuAppRestore;
AuAppRestore au_app_session(AuApp *a, bool unclean, AuSession *s);
/* The URL of the first window when no session was restored: NULL for
 * the home page, or "about:blank". */
const char *au_app_start_url(AuApp *a);

/* Saves the data sets whose change counter moved since they were last
 * saved, once the delay has passed since the first unsaved change; force
 * saves every changed set now (quitting, the application going to the
 * background). Includes cookies, HSTS and web storage once the network
 * is started. */
void au_app_save(AuApp *a, bool force);
/* At quit, while the windows are still open: the history privacy
 * option, the last save, and the Running file removed. */
void au_app_quit(AuApp *a);

/* ---- The network stack ----
 * au_app_net_start makes the AuNet with its root store, cookie jar, HSTS
 * store, memory and disk caches, the web storage service, the loader and
 * page fetcher (registered for http and https) and real downloads,
 * loads Cookies, HSTS and Intermediates, and applies the preferences. */
bool au_app_net_start(AuApp *a, const AuAppNet *net);
/* Once per pass of the event loop: polls the loader and writes one
 * queued disk cache file while nothing loads. */
void au_app_net_idle(AuApp *a);
/* Loads under way or disk cache files queued. */
bool au_app_net_busy(AuApp *a);
int au_app_loads_active(AuApp *a);
/* The preferences changed: applies them to the stack. */
void au_app_prefs_changed(AuApp *a);
/* Clears every cookie and what sites stored with them. */
void au_app_clear_cookies(AuApp *a);
/* Clears the memory and disk caches and the DNS cache. */
void au_app_clear_cache(AuApp *a);
/* The cookie rules in the jar changed (the Cookies window): copied into
 * the cookies.sites preference. */
void au_app_cookie_rules_changed(AuApp *a);
/* The release handler's part: unloads the web storage areas that are
 * saved; returns the bytes freed. */
size_t au_app_release_storage(AuApp *a);
/* The "Network" and "HTTP cache" sections of System Status. */
void au_app_net_status(AuApp *a, AuBuf *out);
/* At quit, after the last page is gone: the cookie and cache privacy
 * options, session cookies dropped, the last save, then everything is
 * freed in reverse order of creation. */
void au_app_net_stop(AuApp *a);

/* NULL before au_app_net_start. */
AuNet *au_app_net(AuApp *a);
struct AuNetLoader *au_app_loader(AuApp *a);
AuCookieJar *au_app_cookie_jar(AuApp *a);
struct AuWebStorage *au_app_storage(AuApp *a);

/* ---- Pieces for tools without a browser ---- */

/* Loads Cookies, HSTS and Intermediates into the stores given (any may
 * be NULL), setting a damaged Cookies or HSTS file aside; the stores are
 * left clean. now: seconds since 1970 UTC. */
void au_app_load_stores(const AuAppFiles *f, AuCookieJar *jar, AuHsts *hsts, AuNet *net, int64_t now);
/* Writes those of the stores that changed. */
void au_app_save_stores(const AuAppFiles *f, AuCookieJar *jar, AuHsts *hsts, AuNet *net, int64_t now);

/* A folder of the profile seen as the file table web storage or the
 * disk cache takes, with timings for System Status. Keep it alive while
 * the service uses it. */
typedef struct AuAppFolder {
    const AuAppFiles *files;
    const char *name;
    size_t max; /* largest file read */
    uint32_t (*us)(void *ctx); /* optional timing clock */
    void *us_ctx;
    uint32_t read_us, write_us; /* totals */
    unsigned reads, writes;
    uint32_t largest_len, largest_us; /* the largest write */
} AuAppFolder;
/* Fills f and makes the folder; false when it cannot be used. */
bool au_app_folder_init(AuAppFolder *f, const AuAppFiles *files, const char *name, size_t max);
struct AuWebStorageFiles;
struct AuDiskCacheFiles;
void au_app_folder_web_storage(AuAppFolder *f, struct AuWebStorageFiles *out);
void au_app_folder_disk_cache(AuAppFolder *f, struct AuDiskCacheFiles *out);

#endif
