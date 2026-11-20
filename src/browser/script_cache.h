/* The script cache: the bytecode of large scripts, kept in the Script
 * Cache folder so that a later page, or a later run of Aura, reads a
 * script instead of compiling it (js/hint_store.h, AuJsCodeStore;
 * js/runtime.c, "Compiled scripts kept between pages").
 *
 * Each script is one file, named by the first 15 bytes of its key in
 * hexadecimal. The file "Index" lists the keys asked for once and not
 * stored yet: the runtime stores a script the second time it compiles
 * it. When the files pass the limit, the oldest written go first.
 *
 * The bytecode is run as it is read and QuickJS does not check it, so a
 * file is used only when its header, key, length and checksum match;
 * that guards against a damaged file, not against someone who can write
 * the folder (who could as well replace the application). The files say
 * which scripts were run, so clearing the cache removes them. */
#ifndef AURA_BROWSER_SCRIPT_CACHE_H
#define AURA_BROWSER_SCRIPT_CACHE_H

#include "../core/core.h"

struct AuJsCodeStore;    /* js/hint_store.h */
struct AuDiskCacheFiles; /* net/disk_cache.h: list, read, write, remove in one folder */

#define AU_SCRIPT_CACHE_INDEX "Index"
#define AU_SCRIPT_CACHE_SEEN 512                        /* keys remembered as asked for once */
#define AU_SCRIPT_CACHE_PENDING_MAX ((size_t)4 << 20)   /* queued bytes before a write is forced */
#define AU_SCRIPT_CACHE_FILE_MAX ((size_t)64 << 20)     /* the largest file read */

typedef struct AuScriptCache AuScriptCache;

/* files NULL: the scripts are kept in memory (tools and tests). The
 * table is copied. max_bytes 0: nothing is stored. */
AuScriptCache *au_script_cache_new(const struct AuDiskCacheFiles *files, size_t max_bytes);
void au_script_cache_free(AuScriptCache *c); /* writes what is queued */
/* Removes the oldest files until the total is at most max_bytes. */
void au_script_cache_set_max(AuScriptCache *c, size_t max_bytes);
void au_script_cache_clear(AuScriptCache *c);
/* The interface the script runtime calls (AuJsSettings.code). */
const struct AuJsCodeStore *au_script_cache_store(AuScriptCache *c);

/* Deferred writes, as the disk cache's (net/disk_cache.h): with deferred
 * on, a stored script is queued and au_script_cache_idle writes up to
 * max_writes of them (true while more wait). Past
 * AU_SCRIPT_CACHE_PENDING_MAX queued bytes a store writes at once. */
void au_script_cache_set_deferred(AuScriptCache *c, bool on);
size_t au_script_cache_pending(const AuScriptCache *c);
bool au_script_cache_idle(AuScriptCache *c, int max_writes);
void au_script_cache_flush(AuScriptCache *c);
/* The Index file: written by au_script_cache_save when it changed. */
bool au_script_cache_dirty(const AuScriptCache *c);
void au_script_cache_save(AuScriptCache *c);

typedef struct AuScriptCacheStats {
    size_t count, bytes; /* scripts stored */
    size_t seen;         /* keys asked for once */
    unsigned hits, misses, stores, damaged;
} AuScriptCacheStats;
void au_script_cache_stats(const AuScriptCache *c, AuScriptCacheStats *out);

/* The Index file's text, for tests and the fuzzer: "# Aura script cache
 * 1", then one key (32 hexadecimal digits) a line, the most recent
 * first. Parsing skips a damaged line and returns false when the header
 * is wrong. */
void au_script_cache_index_serialize(const AuScriptCache *c, AuBuf *out);
bool au_script_cache_index_parse(AuScriptCache *c, const char *data, size_t len);

#endif
