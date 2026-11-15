/* Coroutines: a second stack the engine can run code on and leave in the
 * middle, supplied by the platform (as AuNetOps supplies sockets).
 *
 * Workers use them (src/js/worker.c): each worker's script runs on its own
 * stack, and when it has used its time slice it yields from inside the
 * script engine's interrupt check, so a long computation in a worker does
 * not stop the page. Only one stack runs at a time: resume() does not
 * return until the coroutine yields or ends, so engine code needs no
 * locks, and nothing is shared that the two sides do not hand over.
 *
 * The Mac OS 9 implementation uses the Thread Manager's cooperative
 * threads (src/platform/macos9/coro_mac.c); the host one uses a thread
 * that runs only while its resumer waits (src/platform/host/coro_host.c).
 * Without AuCoroOps a worker runs on the caller's stack, one task at a
 * time, and a long task blocks the page until it returns. */
#ifndef AURA_CORE_CORO_H
#define AURA_CORE_CORO_H

#include <stdbool.h>
#include <stddef.h>

typedef struct AuCoroOps {
    void *ctx;
    /* Makes a coroutine that runs entry(arg) on a stack of at least
     * stack_size bytes when first resumed. NULL when it cannot. */
    void *(*create)(void *ctx, size_t stack_size, void (*entry)(void *arg), void *arg);
    /* Runs co until it yields or entry returns. Returns false once entry
     * has returned (co may then only be destroyed). */
    bool (*resume)(void *ctx, void *co);
    /* On co's stack: goes back to the caller of resume. */
    void (*yield)(void *ctx, void *co);
    /* Frees a coroutine whose entry has returned. */
    void (*destroy)(void *ctx, void *co);
} AuCoroOps;

#endif
