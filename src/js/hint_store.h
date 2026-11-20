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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The code store (runtime.c, "Compiled scripts kept between pages"):
 * the bytecode of large scripts, kept by the embedder so that a later
 * page reads a script instead of compiling it.
 *
 * key names the script: a SHA-256 over the engine build, how the script
 * is compiled, its URL and its text, so no other script has it. get
 * returns the bytecode stored under key as an au_alloc'd block the
 * caller frees, or NULL; *seen says whether get was called with this
 * key before (the runtime stores a script the second time it compiles
 * it). put copies the bytes. The bytecode is run as it is read: the
 * store must return only bytes that put was given, whole. */
#define AU_JS_CODE_KEY 32

typedef struct AuJsCodeStore {
    void *ctx;
    uint8_t *(*get)(void *ctx, const uint8_t key[AU_JS_CODE_KEY], size_t *len, bool *seen);
    void (*put)(void *ctx, const uint8_t key[AU_JS_CODE_KEY], const uint8_t *code, size_t len);
} AuJsCodeStore;

typedef struct AuJsHintStore {
    void *ctx;
    const uint32_t *(*get)(void *ctx, uint32_t len, uint32_t hash, size_t *count);
    void (*add)(void *ctx, uint32_t len, uint32_t hash, uint32_t ofs);
} AuJsHintStore;

#endif
