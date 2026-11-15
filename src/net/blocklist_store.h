/* The blocklists a browser uses: the catalogue, the compiled files kept
 * between runs, and their updates.
 *
 * Aura carries no list. The manager downloads the lists the preferences
 * enable (https only, with Aura's own HTTP client, without cookies and
 * outside the HTTP cache), compiles each to the hash file blocklist.h
 * describes while it arrives, and keeps the compiled file in the
 * platform's store (the Blocklists folder of the preferences folder). A
 * list on disk is attached at start without any request.
 *
 * Updates are conditional requests (If-None-Match, If-Modified-Since),
 * one list at a time, started only from au_block_manager_idle while no
 * page loads, at most once per list per interval (daily, weekly, never;
 * a list that is enabled and missing is fetched whatever the interval).
 * A download that fails, is not https after redirects, parses to nothing
 * or to implausibly few entries leaves the old list in use and is tried
 * again later.
 *
 * The store also holds "Index", a text file with each list's validators
 * and times. */
#ifndef AURA_NET_BLOCKLIST_STORE_H
#define AURA_NET_BLOCKLIST_STORE_H

#include "blocklist.h"
#include "net.h"
#include "../browser/prefs.h"

typedef struct AuBlockStore {
    void *ctx;
    /* Reads the file name whole into a block from mem. */
    bool (*read)(void *ctx, const char *name, const AuBlockMem *mem, uint8_t **data, size_t *len);
    /* Writes it through a temporary file that replaces the old one. */
    bool (*write)(void *ctx, const char *name, const void *data, size_t len);
    void (*remove)(void *ctx, const char *name);
} AuBlockStore;

typedef struct AuBlockCatalogEntry {
    const char *id;          /* also the compiled file's name */
    const char *title;
    const char *url;
    const char *description;
    bool security;           /* a threat list, shown apart from the ad lists */
    uint32_t entries;        /* about how many, for the size shown before a download */
} AuBlockCatalogEntry;
const AuBlockCatalogEntry *au_block_catalog(size_t *n);

typedef enum { AU_BLOCK_IDLE, AU_BLOCK_WAITING, AU_BLOCK_DOWNLOADING, AU_BLOCK_COMPILING } AuBlockListState;

typedef struct AuBlockListInfo {
    char id[32];
    char title[AU_BLOCK_TITLE_MAX];
    const char *url;
    const char *description; /* "" for a list the user added */
    bool catalog, security, enabled, loaded;
    uint32_t entries;        /* when loaded; else the catalogue's estimate */
    uint32_t bytes;          /* memory in use (0 when not loaded) */
    int64_t updated_at;      /* when the list last changed (seconds since 1970), 0 never */
    int64_t checked_at;      /* when the publisher was last asked */
    AuBlockListState state;
    int64_t received;        /* bytes of the download in progress */
    char error[96];          /* the last failure, "" when none */
} AuBlockListInfo;

/* What the last finished compile cost (the debug view). */
typedef struct AuBlockCompileStats {
    char id[32];
    uint32_t entries, text_bytes, file_bytes;
    uint32_t download_ms; /* from the request to the last byte, parsing included */
    uint32_t merge_ms;    /* the merge, spread over idle calls */
    uint32_t merge_calls;
} AuBlockCompileStats;

typedef struct AuBlockManager AuBlockManager;

/* mem may be NULL (au_try_alloc). The manager owns neither net nor bl. */
AuBlockManager *au_block_manager_new(AuNet *net, AuBlocklist *bl, const AuBlockStore *store, const AuBlockMem *mem);
void au_block_manager_free(AuBlockManager *m);
/* Applies the blocking preferences: the switch, the lists enabled (their
 * compiled files are attached from the store, or queued for download),
 * the lists added by URL, the personal entries and the update interval.
 * Call at start and after the preferences change. */
void au_block_manager_apply_prefs(AuBlockManager *m, const AuPrefs *p);
/* Call on every pass of the event loop, saying whether a page is
 * loading. Starts at most one download when nothing loads, and compiles
 * in slices. Returns true while it has work in progress. The AuNet must
 * be pumped by the caller as usual; the clocks are the AuNet's. */
bool au_block_manager_idle(AuBlockManager *m, bool busy);
/* Asks every enabled list's publisher now (still conditional). */
void au_block_manager_update_now(AuBlockManager *m);
size_t au_block_manager_count(const AuBlockManager *m);
bool au_block_manager_info(const AuBlockManager *m, size_t i, AuBlockListInfo *out);
/* Changes whenever a list's state does (to redraw a preferences pane). */
uint32_t au_block_manager_generation(const AuBlockManager *m);
void au_block_manager_compile_stats(const AuBlockManager *m, AuBlockCompileStats *out);

#endif
