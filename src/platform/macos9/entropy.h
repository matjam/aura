/* The Mac OS 9 entropy pool behind AuNetOps.entropy.
 *
 * Mac OS 9 has no random number generator a program can trust, so Aura
 * gathers its own: timing jitter of the main loop, the time and content
 * of user input, the mouse position, clocks and free memory, all mixed
 * into a SHA-256 pool. Each input is credited with a conservative number
 * of bits; au_mac_entropy_read refuses to produce output until the pool
 * has been credited with AU_MAC_ENTROPY_THRESHOLD bits, so TLS waits for
 * it. A seed file in the Aura preferences folder carries pool state from
 * one run to the next; it is rewritten at start (so a crash never reuses
 * a seed), when the pool first reaches the threshold, and at exit.
 *
 * Call every function from the main task only (never from a notifier,
 * Time Manager task or MP task). */
#ifndef AURA_MAC_ENTROPY_H
#define AURA_MAC_ENTROPY_H

#include <Multiverse.h>

#include <stdbool.h>
#include <stddef.h>

/* Bits the pool must be credited with before it produces output. */
#define AU_MAC_ENTROPY_THRESHOLD 128

typedef struct AuMacEntropy AuMacEntropy;

/* Reads and rewrites the seed file, mixes the start-up sources and runs a
 * short jitter collection (about 50 ms). */
AuMacEntropy *au_mac_entropy_new(void);
/* Writes the seed file and frees the pool. */
void au_mac_entropy_free(AuMacEntropy *e);
/* Every event WaitNextEvent returns, null events included. */
void au_mac_entropy_event(AuMacEntropy *e, const EventRecord *ev);
/* Once per pass of the event loop: loop timing jitter and the mouse. */
void au_mac_entropy_idle(AuMacEntropy *e);
/* Mixes other material; credit is in 1/64 bits (0 for anything an
 * observer could know, such as network timings). */
void au_mac_entropy_add(AuMacEntropy *e, const void *data, size_t len, unsigned credit);
/* Fills out with len bytes and returns true, or returns false while the
 * pool is below the threshold. Output never repeats: the pool state moves
 * on after every read. */
bool au_mac_entropy_read(AuMacEntropy *e, void *out, size_t len);
/* Bits credited so far (capped at 256). */
unsigned au_mac_entropy_bits(const AuMacEntropy *e);
/* Where the credit came from, for diagnostics: bits from the seed file,
 * the start-up jitter run, loop jitter and user input. */
void au_mac_entropy_sources(const AuMacEntropy *e, unsigned *seed, unsigned *startup, unsigned *jitter,
                            unsigned *input);

#endif
