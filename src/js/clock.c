/* The embedder's clock for Date. See clock.h. */
#include "clock.h"

#include <quickjs.h>
#include <stddef.h>

static AuJsClock g_clock;

static int64_t clock_now(void *opaque)
{
    (void)opaque;
    return g_clock.wall_ms(g_clock.ctx);
}

/* QuickJS wants UTC minus local in whole minutes. */
static int clock_tz(void *opaque, int64_t time_ms)
{
    (void)opaque;
    return (int)(-g_clock.utc_offset_s(g_clock.ctx, time_ms) / 60);
}

void au_js_set_clock(const AuJsClock *clock)
{
    if (!clock) {
        g_clock = (AuJsClock){0};
        JS_SetTimeSource(NULL, NULL, NULL);
        return;
    }
    g_clock = *clock;
    JS_SetTimeSource(g_clock.wall_ms ? clock_now : NULL, g_clock.utc_offset_s ? clock_tz : NULL, NULL);
}

int64_t au_js_clock_now_ms(void) { return JS_GetTimeNow(); }

int32_t au_js_clock_utc_offset_s(int64_t utc_ms) { return -(int32_t)JS_GetTimeZoneOffset(utc_ms) * 60; }
