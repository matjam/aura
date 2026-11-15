/* The HTTP cache (RFC 9111, a private browser cache): an in-memory store
 * with a byte budget and least-recently-used eviction, plus an optional
 * second-level storage interface (for a disk cache).
 *
 * The client (http.c) uses it like this for GET requests:
 *   - au_cache_lookup: FRESH entries are delivered without a request;
 *     STALE entries with a validator (ETag, Last-Modified) are
 *     revalidated with If-None-Match / If-Modified-Since, and a 304
 *     updates the entry (au_cache_update) and delivers it.
 *   - au_cache_begin on a final response; au_cache_write for each decoded
 *     body chunk; au_cache_commit when the body is complete (or
 *     au_cache_abort). Bodies are stored decoded, without
 *     Content-Encoding.
 *   - au_cache_invalidate after a successful unsafe request (POST, ...)
 *     for its URL and its same-origin Location / Content-Location.
 *
 * What is stored: final responses to GET with status 200, 203, 204, 300,
 * 301, 308, 404, 405, 410, 414 or 501, unless the response says
 * no-store, the request said no-store, the request carried Authorization
 * (unless the response says public, must-revalidate or s-maxage), the
 * response has Vary: *, or the body is larger than the per-entry limit
 * (an eighth of the budget, at most 8 MB). Set-Cookie and hop-by-hop
 * fields are not kept.
 *
 * Freshness: max-age, else Expires minus Date, else (for the statuses
 * above, with Last-Modified) 10 % of the time since Last-Modified, capped
 * at one day. no-cache, and a request's no-cache or max-age=0, force
 * revalidation; must-revalidate forbids serving stale; immutable entries
 * are not revalidated by a reload of the page (the client decides).
 * Vary is honoured by keeping the request's values of the named fields.
 *
 * Entries returned by au_cache_lookup are reference-counted; release
 * them with au_cache_entry_release. Eviction never frees an entry still
 * referenced. No global state; one cache per application. */
#ifndef AURA_NET_CACHE_H
#define AURA_NET_CACHE_H

#include "net.h"

typedef struct AuCacheEntry AuCacheEntry;
typedef struct AuCacheWriter AuCacheWriter;

/* Optional second-level storage. Keys are URLs without fragment (plus a
 * Vary suffix); values are opaque blobs from au_cache_entry_serialize.
 * The platform implements it on files; a failure just means a miss. */
typedef struct AuCacheStorage {
    void *ctx;
    void (*store)(void *ctx, const char *key, const void *data, size_t len);
    bool (*load)(void *ctx, const char *key, AuBuf *out);
    void (*remove)(void *ctx, const char *key);
} AuCacheStorage;

AuHttpCache *au_cache_new(size_t max_bytes);
void au_cache_free(AuHttpCache *c);
void au_cache_set_max(AuHttpCache *c, size_t max_bytes); /* evicts down to it */
void au_cache_set_storage(AuHttpCache *c, const AuCacheStorage *s);
void au_cache_clear(AuHttpCache *c);
size_t au_cache_bytes(const AuHttpCache *c);
size_t au_cache_count(const AuHttpCache *c);
size_t au_cache_entry_limit(const AuHttpCache *c);

typedef enum { AU_CACHE_MISS, AU_CACHE_FRESH, AU_CACHE_STALE } AuCacheState;

/* Looks up url (serialized without fragment) for a request with the given
 * headers at time now (seconds). FRESH: usable as is. STALE: usable after
 * revalidation (the entry has a validator), or not at all. On FRESH or
 * STALE *out holds a reference. */
AuCacheState au_cache_lookup(AuHttpCache *c, const char *url, const AuHeaders *req, int64_t now, AuCacheEntry **out);
void au_cache_entry_release(AuHttpCache *c, AuCacheEntry *e);

int au_cache_entry_status(const AuCacheEntry *e);
const char *au_cache_entry_reason(const AuCacheEntry *e);
const AuHeaders *au_cache_entry_headers(const AuCacheEntry *e);
const uint8_t *au_cache_entry_body(const AuCacheEntry *e, size_t *len);
const char *au_cache_entry_etag(const AuCacheEntry *e);          /* NULL when none */
const char *au_cache_entry_last_modified(const AuCacheEntry *e); /* NULL when none */
bool au_cache_entry_immutable(const AuCacheEntry *e);

/* Storing. begin returns NULL when the response may not be stored. */
AuCacheWriter *au_cache_begin(AuHttpCache *c, const char *url, const AuHeaders *req, int status, const char *reason,
                              const AuHeaders *resp, int64_t request_time, int64_t response_time);
/* Returns false (and aborts the writer) when the body outgrows the limit. */
bool au_cache_write(AuCacheWriter *w, const void *data, size_t len);
void au_cache_commit(AuCacheWriter *w);
void au_cache_abort(AuCacheWriter *w);

/* A 304 for entry e: merges the new header fields and resets its age. */
void au_cache_update(AuHttpCache *c, AuCacheEntry *e, const AuHeaders *resp304, int64_t request_time,
                     int64_t response_time);
/* Removes every entry for url (all Vary variants). */
void au_cache_invalidate(AuHttpCache *c, const char *url);

/* ---- The policy, exposed for tests ---- */
typedef struct AuCacheControl {
    bool no_store, no_cache, must_revalidate, public_, private_, immutable;
    int64_t max_age;   /* -1 when absent */
    int64_t s_maxage;  /* -1 when absent */
    int64_t max_stale; /* request: -1 absent; INT64_MAX for "max-stale" without a value */
    int64_t min_fresh; /* request: -1 when absent */
} AuCacheControl;
void au_cache_control_parse(const AuHeaders *h, AuCacheControl *cc);
/* Freshness lifetime in seconds (0 when none; heuristic when allowed). */
int64_t au_cache_freshness_lifetime(int status, const AuHeaders *resp);
/* Current age (RFC 9111 4.2.3) at now of a response received between
 * request_time and response_time. */
int64_t au_cache_current_age(const AuHeaders *resp, int64_t request_time, int64_t response_time, int64_t now);

/* Serialization of one entry for AuCacheStorage (and its parser, which
 * returns NULL for damaged data). The format: "AURC1\n", then
 * status, reason, url, times and the stored request (Vary) and response
 * fields as length-prefixed strings, then the body. */
void au_cache_entry_serialize(const AuCacheEntry *e, AuBuf *out);
AuCacheEntry *au_cache_entry_parse(const void *data, size_t len);
void au_cache_entry_free(AuCacheEntry *e); /* for a parsed entry not given to a cache */

#endif
