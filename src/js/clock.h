/* The clock and time zone script sees through Date.
 *
 * Without a call to au_js_set_clock, Date reads the C library: right on
 * the host, wrong on Mac OS 9, where newlib knows neither the Mac's
 * clock nor its time zone setting. An embedder with a clock of its own
 * (the Mac OS 9 application, a test with a fixed time) installs it
 * once, before or after runtimes exist. The clock is process-wide, as
 * the time is: every runtime uses it.
 *
 * performance.now() and timers keep AuJsHooks.now_ms, a monotonic
 * clock; performance.timeOrigin is Date.now() when the runtime starts. */
#ifndef AURA_JS_CLOCK_H
#define AURA_JS_CLOCK_H

#include <stdint.h>

typedef struct AuJsClock {
    void *ctx;
    /* UTC milliseconds since 1970-01-01. */
    int64_t (*wall_ms)(void *ctx);
    /* Seconds local time is east of UTC at utc_ms (-28800 for UTC-8).
     * NULL keeps the C library's zone. */
    int32_t (*utc_offset_s)(void *ctx, int64_t utc_ms);
} AuJsClock;

/* Installs the clock (copied); NULL goes back to the C library. */
void au_js_set_clock(const AuJsClock *clock);
/* What Date.now() returns at this moment. */
int64_t au_js_clock_now_ms(void);
/* Seconds east of UTC that Date's local-time methods use at utc_ms. */
int32_t au_js_clock_utc_offset_s(int64_t utc_ms);

#endif
