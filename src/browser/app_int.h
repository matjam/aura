/* The application services' state, shared by app.c (files, loading and
 * saving) and app_net.c (the network stack). See app.h. */
#ifndef AURA_BROWSER_APP_INT_H
#define AURA_BROWSER_APP_INT_H

#include "app.h"

#include "../net/cache.h"
#include "../net/disk_cache.h"
#include "../net/loader.h"
#include "../net/roots.h"
#include "../net/transfer.h"
#include "../net/web_storage.h"

typedef enum { AU_APP_F_PREFS, AU_APP_F_BOOKMARKS, AU_APP_F_HISTORY, AU_APP_F_SESSION, AU_APP_F_DOWNLOADS,
               AU_APP_F_COUNT } AuAppFileId;

typedef struct AuAppSaveState {
    uint32_t saved_gen;
    uint32_t first_change_ms;
    bool pending;
} AuAppSaveState;

struct AuApp {
    AuBrowser *b;
    AuAppPort port;
    bool clock_installed;

    /* Saving the browser's data sets. */
    AuAppSaveState save[AU_APP_F_COUNT];
    uint32_t saved_session_gen; /* session_gen when the Session file was last written */
    uint32_t favicons_saved_ms;

    /* The script clock's UTC offset, read at most once a minute. */
    bool offset_read;
    uint32_t offset_read_ms;
    int32_t offset;

    /* The network stack (app_net.c). */
    bool net_started;
    AuAppNet np;
    AuNet *net;
    AuRootStore *roots;
    AuCookieJar *jar;
    AuHsts *hsts;
    AuHttpCache *cache;
    AuDiskCache *disk;
    AuAppFolder cache_folder, ls_folder;
    AuNetLoader *loader;
    AuNetPageFetcher *pf;
    AuNetTransferSource *xfer;
    bool cookies_pending, hsts_pending;
    uint32_t cookies_first_ms, hsts_first_ms;
    AuWebStorage *storage;
    bool storage_pending;
    uint32_t storage_gen, storage_change_ms, storage_first_ms;
    struct AuScriptHints *hints; /* script_hints.h */
    uint32_t hints_saved_ms;     /* when the Script Hints file was last written */
};

/* app.c */
uint32_t au_app_ms(AuApp *a);
int64_t au_app_wall_time(AuApp *a);
/* Reads a file of the profile folder (NUL-terminated, au_free). */
bool au_app_read_file(const AuAppFiles *f, const char *name, char **data, size_t *len);
bool au_app_write_file(const AuAppFiles *f, const char *name, const AuBuf *data);
/* Renames a file that did not parse to "<name> (damaged)". */
bool au_app_set_aside(const AuAppFiles *f, const char *folder, const char *name);

/* app_net.c: Cookies, HSTS, Intermediates and web storage, when due. */
void au_app_net_save(AuApp *a, bool force);

#endif
