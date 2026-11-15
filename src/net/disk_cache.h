/* A disk cache behind AuCacheStorage: one file per entry in a folder the
 * platform provides, with an in-memory index of names, sizes and use
 * order, and a byte limit (least recently used files removed first).
 *
 * File names are "c" and 16 hex digits of a 64-bit FNV-1a hash of the
 * key (18 characters, within HFS's 31). A file holds "AURD1\n", the key,
 * "\n", then the entry blob from au_cache_entry_serialize; a file whose
 * key differs (a hash collision) or that is damaged is a miss. The
 * platform writes each file safely (temporary file, then rename), so a
 * crash leaves either the old file or the new one; anything that fails to
 * parse later is removed.
 *
 * The index is built from the folder's listing on first use, ordered by
 * the files' modification times, oldest first. */
#ifndef AURA_NET_DISK_CACHE_H
#define AURA_NET_DISK_CACHE_H

#include "cache.h"

typedef struct AuDiskCacheFiles {
    void *ctx;
    /* Calls each(each_ctx, name, size, mtime) for every file in the cache
     * folder (mtime in any unit that orders by age). */
    void (*list)(void *ctx, void (*each)(void *each_ctx, const char *name, size_t size, uint32_t mtime),
                 void *each_ctx);
    bool (*read)(void *ctx, const char *name, AuBuf *out);
    bool (*write)(void *ctx, const char *name, const void *data, size_t len);
    void (*remove)(void *ctx, const char *name);
} AuDiskCacheFiles;

typedef struct AuDiskCache AuDiskCache;

AuDiskCache *au_disk_cache_new(const AuDiskCacheFiles *files, size_t max_bytes);
void au_disk_cache_free(AuDiskCache *d);
/* Fills an AuCacheStorage for au_cache_set_storage. */
void au_disk_cache_storage(AuDiskCache *d, AuCacheStorage *out);
/* Removes files until the total is at most max_bytes (0 removes all). */
void au_disk_cache_set_max(AuDiskCache *d, size_t max_bytes);
void au_disk_cache_clear(AuDiskCache *d);
size_t au_disk_cache_bytes(AuDiskCache *d);
size_t au_disk_cache_count(AuDiskCache *d);
/* The file name for a key (at least 19 bytes). */
void au_disk_cache_name(const char *key, char out[19]);

#endif
