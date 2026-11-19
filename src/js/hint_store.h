/* The hint store (AuJsSettings.hints; js/runtime.c, "Script hints"),
 * in a header of its own so that the embedder's store needs only this.
 *
 * A script is named by its length in bytes and a 32-bit FNV-1a hash of
 * its bytes (1 when the hash is 0); a function by the offset of its
 * first byte in the script. get returns the script's offsets, sorted
 * and ascending, or NULL; the array stays valid until the next call to
 * add. A wrong answer (a hash collision, a damaged file) costs only
 * memory: an offset that starts no function is ignored, and a function
 * compiled with its script works as one compiled at its first call. */
#ifndef AURA_JS_HINT_STORE_H
#define AURA_JS_HINT_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct AuJsHintStore {
    void *ctx;
    const uint32_t *(*get)(void *ctx, uint32_t len, uint32_t hash, size_t *count);
    void (*add)(void *ctx, uint32_t len, uint32_t hash, uint32_t ofs);
} AuJsHintStore;

#endif
