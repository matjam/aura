/* The ordered map a web storage area is made of: byte-string keys and
 * values in insertion order (web_storage.c). The strings are what the
 * script engine gives for a JavaScript string: UTF-8 in which a lone
 * surrogate is three bytes, so every sequence of UTF-16 code units
 * comes back as it went in. Depends on src/core only, so that src/js can
 * use it for a document without a storage service. */
#ifndef AURA_NET_STORAGE_MAP_H
#define AURA_NET_STORAGE_MAP_H

#include "../core/core.h"

typedef struct AuStorageMap AuStorageMap;

AuStorageMap *au_storage_map_new(void);
void au_storage_map_free(AuStorageMap *m);
uint32_t au_storage_map_count(const AuStorageMap *m);
/* Two bytes per UTF-16 code unit of every key and value. */
uint32_t au_storage_map_bytes(const AuStorageMap *m);
/* The i-th key in insertion order; false past the end. The pointers of
 * key and get are valid until the map next changes. */
bool au_storage_map_key(AuStorageMap *m, uint32_t i, const char **k, size_t *klen);
bool au_storage_map_get(const AuStorageMap *m, const char *k, size_t klen, const char **v, size_t *vlen);
/* Sets a value (a new key goes last). False when the value was the same. */
bool au_storage_map_set(AuStorageMap *m, const char *k, size_t klen, const char *v, size_t vlen);
bool au_storage_map_remove(AuStorageMap *m, const char *k, size_t klen);
void au_storage_map_clear(AuStorageMap *m);
/* The UTF-16 length of n bytes of such text. */
uint32_t au_storage_utf16_units(const char *s, size_t n);

#endif
