/* One web storage operation, as it travels from script
 * (AuJsHooks.storage, js.h) through the document view
 * (AuDocViewCallbacks.storage) to the embedder's storage service. A
 * header of its own because the embedder's page code cannot include
 * js.h. Strings are the script engine's UTF-8, in which a lone surrogate
 * is three bytes, with their lengths (they may hold NUL). */
#ifndef AURA_JS_STORAGE_CALL_H
#define AURA_JS_STORAGE_CALL_H

#include <stddef.h>
#include <stdint.h>

enum { AU_JS_STORAGE_LOCAL, AU_JS_STORAGE_SESSION };
enum {
    AU_JS_STORAGE_ACCESS,   /* may the document use this kind at all */
    AU_JS_STORAGE_LENGTH,   /* -> length */
    AU_JS_STORAGE_KEY,      /* index -> out (MISSING past the end) */
    AU_JS_STORAGE_GET,      /* key -> out (MISSING when absent) */
    AU_JS_STORAGE_SET,      /* key, value (QUOTA when it does not fit) */
    AU_JS_STORAGE_REMOVE,   /* key */
    AU_JS_STORAGE_CLEAR,
    AU_JS_STORAGE_ESTIMATE, /* -> usage, quota (bytes) */
    AU_JS_STORAGE_MATCH     /* is key the name of the document's area of this kind (OK), or not (MISSING) */
};
enum { AU_JS_STORAGE_OK, AU_JS_STORAGE_MISSING, AU_JS_STORAGE_QUOTA, AU_JS_STORAGE_DENIED };

typedef struct AuJsStorageCall {
    uint8_t kind, op;
    const char *key, *value;
    size_t key_len, value_len;
    uint32_t index;
    /* Results. out is valid until the next storage call. */
    const char *out;
    size_t out_len;
    uint32_t length;
    double usage, quota;
    /* The caller, filled in on the way to the service: the document's
     * origin (never an opaque one: those are refused before the hook) and
     * URL by src/js; the rest by the document view. */
    const char *origin, *url;
    const char *top_url; /* the top-level document's URL for a nested document, else NULL */
    const void *writer;  /* names the writing document to the service's listener */
} AuJsStorageCall;

#endif
