/* Web storage (the Storage standard's localStorage and sessionStorage):
 * one service per application, beside the cookie jar. docs/web-storage.md
 * describes it.
 *
 * An area is an ordered map of byte strings (AuStorageMap). The strings
 * are what the script engine gives for a JavaScript string: UTF-8 in
 * which a lone surrogate is three bytes (WTF-8), so every sequence of
 * UTF-16 code units comes back as it went in. Sizes are counted as the
 * standard's browsers count them: two bytes per UTF-16 code unit of
 * keys and values.
 *
 * Areas are named by a storage key:
 *   - the document's origin ("https://example.com", "http://a.test:8080",
 *     "file://" for every file: document), or
 *   - for a document in a third-party context (its registrable domain
 *     differs from the top-level document's), the origin, '^' and the
 *     top-level site ("https://widget.example^https://news.example"), so
 *     a cross-site frame has an area per embedding site.
 * A local area is kept for the session only (never written) when the
 * site's cookie rule is "session only", or when it is a third-party area
 * and the cookie preferences block third-party cookies. Access is
 * denied (script gets SecurityError) for an opaque origin, when cookies
 * are off and the site has no "always allow" rule, and for a site whose
 * rule is "block". The rules come from the cookie jar
 * (au_web_storage_set_cookie_jar); without one everything is allowed and
 * persistent.
 *
 * Session areas are named by a namespace (one per tab, a number from
 * au_web_storage_ns_new) and the storage key.
 *
 * Limits: AU_WEB_STORAGE_AREA_BYTES per area and AU_WEB_STORAGE_TOTAL_BYTES
 * over all areas (au_web_storage_set_limits changes both); a write over
 * either fails with AU_STORAGE_QUOTA, after the least recently used
 * session-only areas have been dropped to make room for the total.
 * Persistent areas are never dropped. AU_WEB_STORAGE_RESIDENT_BYTES of
 * area data stay in memory: beyond it, unchanged persistent areas are
 * unloaded (and read back when next used) and changed ones are written
 * first.
 *
 * Files (AuWebStorageFiles; names are at most 8 characters, plus the
 * platform's ".tmp"): "Index", text, one area per line
 *     # Aura local storage 1
 *     <file>\t<bytes>\t<items>\t<storage key>
 * and one file per persistent area, named by 8 hex digits from a hash of
 * its storage key, all integers big-endian:
 *     "AuLS"  u32 version (1)
 *     u32 length, the storage key
 *     u32 item count
 *     per item: u32 length, key bytes, u32 length, value bytes
 *     u32 FNV-1a of every byte before it
 * A file that is short, long, of another key or with a wrong sum is
 * damaged: the area starts empty and the platform sets the file aside.
 * Every length is checked against what is left of the file. */
#ifndef AURA_NET_WEB_STORAGE_H
#define AURA_NET_WEB_STORAGE_H

#include "cookies.h"
#include "storage_map.h"
#include "../js/storage_call.h"

#define AU_WEB_STORAGE_AREA_BYTES (5u << 20)
#define AU_WEB_STORAGE_TOTAL_BYTES (50u << 20)
#define AU_WEB_STORAGE_RESIDENT_BYTES (16u << 20)
#define AU_WEB_STORAGE_MAX_AREAS 4096
/* The largest area file read (a 5 MB area of three-byte characters). */
#define AU_WEB_STORAGE_FILE_MAX (12u << 20)
/* Session storage written into the Session file: at most this much per
 * tab, an area that does not fit being left out. */
#define AU_WEB_STORAGE_SESSION_SAVE_BYTES (256u << 10)

/* ---- The service ---- */

typedef struct AuWebStorage AuWebStorage;
typedef struct AuStorageArea AuStorageArea;

enum { AU_STORAGE_LOCAL, AU_STORAGE_SESSION };
typedef enum { AU_STORAGE_OK, AU_STORAGE_QUOTA, AU_STORAGE_DENIED } AuStorageResult;

/* The platform's files, all in one folder. read appends the file to out.
 * write replaces the file safely (a temporary file swapped in). damaged
 * (optional) sets a file that did not parse aside. */
typedef struct AuWebStorageFiles {
    void *ctx;
    bool (*read)(void *ctx, const char *name, AuBuf *out);
    bool (*write)(void *ctx, const char *name, const void *data, size_t len);
    void (*remove)(void *ctx, const char *name);
    void (*damaged)(void *ctx, const char *name);
} AuWebStorageFiles;

/* A change script made, told to the listener before it is applied so
 * that the old value can be read. key is NULL for clear(); old_value is
 * NULL for a new key; new_value is NULL for a removal. writer and url
 * are the caller's (the document that wrote, and its address). */
typedef struct AuStorageChange {
    int kind;
    uint32_t ns; /* the namespace of a session area, else 0 */
    const char *storage_key;
    const char *key, *old_value, *new_value;
    size_t key_len, old_len, new_len;
    const char *url;
    const void *writer;
} AuStorageChange;

AuWebStorage *au_web_storage_new(void);
void au_web_storage_free(AuWebStorage *ws);
void au_web_storage_set_cookie_jar(AuWebStorage *ws, AuCookieJar *jar);
/* 0 keeps a limit as it is. */
void au_web_storage_set_limits(AuWebStorage *ws, uint32_t area_bytes, uint32_t total_bytes, uint32_t resident_bytes);
/* Attaches the files and reads the index; areas load when first used. */
void au_web_storage_set_files(AuWebStorage *ws, const AuWebStorageFiles *files);
void au_web_storage_set_listener(AuWebStorage *ws, void (*fn)(void *ctx, const AuStorageChange *c), void *ctx);

/* The storage key of a document: its origin as script sees it and, for
 * a nested document, the top-level document's URL (NULL at the top
 * level). DENIED when the document may not use storage. session_only
 * (may be NULL) says whether its local area is kept for the session
 * only. */
AuStorageResult au_web_storage_key(AuWebStorage *ws, const char *origin, const char *top_url, AuBuf *key,
                                   bool *session_only);
/* The area of a document (ns names the tab for AU_STORAGE_SESSION and is
 * ignored for AU_STORAGE_LOCAL). *area is NULL when the area does not
 * exist yet and create is false: it is then empty. A persistent area is
 * read from its file the first time. The pointer is valid until the next
 * call that may remove an area (a write, a clear, a removal). */
AuStorageResult au_web_storage_open(AuWebStorage *ws, int kind, uint32_t ns, const char *origin, const char *top_url,
                                    bool create, AuStorageArea **area);
/* The area's contents, for reading. */
AuStorageMap *au_storage_area_map(AuStorageArea *a);
const char *au_storage_area_key(const AuStorageArea *a);
/* Changes, each told to the listener. set returns AU_STORAGE_QUOTA when
 * the area or the total would grow past its limit. An area left empty
 * is removed (the pointer is then gone). */
AuStorageResult au_web_storage_set(AuWebStorage *ws, AuStorageArea *a, const char *k, size_t klen, const char *v,
                                   size_t vlen, const char *url, const void *writer);
void au_web_storage_remove(AuWebStorage *ws, AuStorageArea *a, const char *k, size_t klen, const char *url,
                           const void *writer);
void au_web_storage_clear_area(AuWebStorage *ws, AuStorageArea *a, const char *url, const void *writer);
/* The per-area limit, for navigator.storage.estimate(). */
uint32_t au_web_storage_area_quota(const AuWebStorage *ws);
/* One operation as script asks for it (AuJsStorageCall,
 * src/js/storage_call.h, with the document's origin, URL and top-level
 * URL filled in) on the document's area, ns naming its tab; returns an
 * AU_JS_STORAGE_* result. What an embedder's storage callback calls. */
int au_web_storage_call(AuWebStorage *ws, uint32_t ns, AuJsStorageCall *call);

/* ---- Session namespaces (one per tab) ---- */
uint32_t au_web_storage_ns_new(AuWebStorage *ws);
void au_web_storage_ns_free(AuWebStorage *ws, uint32_t ns);
/* Copies every area of from into to (a duplicated or opened tab). */
void au_web_storage_ns_copy(AuWebStorage *ws, uint32_t from, uint32_t to);
/* The namespace's areas as lines for the Session file, each
 *     storage\t<storage key>\t<key>\t<value>...\n
 * with '%', tab, CR, LF and NUL in a field written as %25, %09, %0D,
 * %0A, %00; at most AU_WEB_STORAGE_SESSION_SAVE_BYTES in all. */
void au_web_storage_ns_serialize(const AuWebStorage *ws, uint32_t ns, AuBuf *out);
/* Reads one such line's text after "storage\t" into the namespace.
 * False for a damaged line (nothing is added). */
bool au_web_storage_ns_restore(AuWebStorage *ws, uint32_t ns, const char *line, size_t len);
/* A counter that moves when any session area changes (the session file
 * is then out of date). */
uint32_t au_web_storage_session_gen(const AuWebStorage *ws);

/* ---- Saving ----
 * pending counts what has to be written: changed persistent areas,
 * files to delete, and the index. change_gen moves with every change
 * to a persistent area. save writes up to max areas (0: all) and the
 * rest of what is pending, and returns the number of files written or
 * removed; an area whose write fails stays pending. */
uint32_t au_web_storage_pending(const AuWebStorage *ws);
uint32_t au_web_storage_change_gen(const AuWebStorage *ws);
uint32_t au_web_storage_save(AuWebStorage *ws, uint32_t max);
/* Unloads the persistent areas that need no write (low memory). Returns
 * the bytes freed. */
size_t au_web_storage_release(AuWebStorage *ws);

/* ---- The manager's view (the Cookies window) and clearing ---- */
typedef struct AuStorageAreaInfo {
    const char *key; /* the storage key */
    int kind;
    uint32_t ns;
    uint32_t bytes;
    uint32_t items;
    bool session_only; /* a local area kept for the session only */
} AuStorageAreaInfo;
size_t au_web_storage_count(const AuWebStorage *ws);
bool au_web_storage_info(const AuWebStorage *ws, size_t i, AuStorageAreaInfo *out);
/* The site a storage key belongs to, as the cookie manager names sites
 * (au_cookie_site_key of the origin's host; "local files" for file:). */
void au_web_storage_site(const char *storage_key, AuBuf *out);
void au_web_storage_remove_at(AuWebStorage *ws, size_t i);
/* Removes every area, local and session, of a site; returns how many. */
size_t au_web_storage_remove_site(AuWebStorage *ws, const char *site);
/* Removes everything (Clear Cookies, clearing at quit). */
void au_web_storage_clear(AuWebStorage *ws);
/* Moves whenever the set of areas or a size changes. */
uint32_t au_web_storage_generation(const AuWebStorage *ws);

typedef struct AuWebStorageStats {
    uint32_t local_areas, session_only_areas, session_areas;
    uint32_t loaded; /* local areas in memory */
    size_t bytes;    /* of every area, by the quota's count */
    size_t resident; /* bytes of area data in memory */
    uint32_t pending;
    uint32_t reads, writes, damaged; /* files, since start */
} AuWebStorageStats;
void au_web_storage_stats(const AuWebStorage *ws, AuWebStorageStats *out);

/* Parses an area file (the fuzzer's entry too). True and the items in m
 * when it is whole and names key. */
bool au_web_storage_parse_file(const char *data, size_t len, const char *key, AuStorageMap *m);

#endif
