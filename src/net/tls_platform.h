/* What the TLS library (Mbed TLS, third_party/mbedtls) asks of its
 * embedder, supplied from Aura's own services: memory, the clocks and
 * entropy. The library makes no operating system call.
 *
 * This is the one place in the network stack with process-wide state:
 * the library's allocator hook, its key store and its random generator
 * are global, so the entropy pool and the clock values here are too.
 * Everything runs on the main task. */
#ifndef AURA_NET_TLS_PLATFORM_H
#define AURA_NET_TLS_PLATFORM_H

#include "net.h"

/* Mixes fresh seed material from the platform's entropy source
 * (AuNetOps.entropy) into the pool the library's random generator seeds
 * itself from, and starts the library on the first call. Returns false
 * when the library could not start (out of memory). */
bool au_tls_entropy_feed(const uint8_t *seed, size_t len);
/* True once au_tls_entropy_feed has succeeded: TLS and au_tls_random can
 * be used. */
bool au_tls_entropy_ready(void);
/* Random bytes from the library's generator. False before the first
 * au_tls_entropy_feed, or when the generator fails. */
bool au_tls_random(uint8_t *out, size_t len);
/* The clocks the library reads: the wall clock (seconds since 1970 UTC)
 * for certificate validity, and a millisecond clock for session ticket
 * ages. tls.c sets the wall clock to each connection's validation time
 * before it calls the library; the stack calls au_tls_tick on every pump. */
void au_tls_set_wall(int64_t now);
void au_tls_tick(uint32_t now_ms);
/* Installs the allocator and clock hooks (idempotent; tls.c calls it). */
void au_tls_platform_init(void);
/* Bytes the library holds now and the most it has held, for the
 * Performance window and the tests. */
void au_tls_memory(size_t *live, size_t *peak);
/* Test hook: the next n allocations by the library succeed, then every
 * one fails until this is called again with a negative n. */
void au_tls_fail_alloc_after(long n);

#endif
