/* Clocks for the network stack on Mac OS 9. See clock_mac.h. */
#include "clock_mac.h"

/* Microseconds and ReadLocation are in InterfaceLib; the Multiversal
 * headers leave them out. */
typedef struct {
    uint32_t hi, lo;
} AuMacClockWide;
extern pascal void Microseconds(AuMacClockWide *us);

typedef struct {
    int32_t latitude, longitude; /* Fract */
    int32_t gmt_delta;           /* high byte: daylight saving flags */
} AuMacLocation;
extern pascal void ReadLocation(AuMacLocation *loc);

uint32_t au_mac_clock_ms(void)
{
    AuMacClockWide us;
    Microseconds(&us);
    uint64_t v = ((uint64_t)us.hi << 32) | us.lo;
    return (uint32_t)(v / 1000u);
}

uint32_t au_mac_clock_us(void)
{
    AuMacClockWide us;
    Microseconds(&us);
    return us.lo;
}

static uint32_t read_gmt_delta(void)
{
    AuMacLocation loc;
    loc.latitude = loc.longitude = 0;
    loc.gmt_delta = 0;
    ReadLocation(&loc);
    return (uint32_t)loc.gmt_delta;
}

bool au_mac_daylight_saving(void)
{
    /* The top byte of gmtDelta is dlsDelta: bit 7 set while daylight
     * saving is on. */
    return (read_gmt_delta() & 0x80000000u) != 0;
}

int32_t au_mac_gmt_offset(void)
{
    /* The low 24 bits are the signed offset in seconds; the Date & Time
     * control panel keeps the daylight saving hour in it while daylight
     * saving is on. */
    int32_t d = (int32_t)(read_gmt_delta() & 0x00FFFFFF);
    if (d & 0x00800000)
        d -= 0x01000000;
    return d;
}

int64_t au_mac_wall_time(void)
{
    unsigned long secs = 0;
    ReadDateTime(&secs);
    /* Local seconds since 1904-01-01 to UTC seconds since 1970-01-01. */
    return (int64_t)secs - (int64_t)au_mac_gmt_offset() - (int64_t)2082844800;
}

int64_t au_mac_wall_ms(void)
{
    /* The system clock counts whole seconds. The milliseconds come from
     * the time since startup, tied to the system clock at the first call
     * and tied again whenever the two are more than 2 seconds apart (the
     * user set the clock or the zone, or the machine slept). */
    static bool have;
    static int64_t base_ms;  /* wall time at the anchor */
    static uint64_t base_us; /* Microseconds at the anchor */
    AuMacClockWide w;
    Microseconds(&w);
    uint64_t us = ((uint64_t)w.hi << 32) | w.lo;
    int64_t secs_ms = au_mac_wall_time() * 1000;
    if (have) {
        int64_t t = base_ms + (int64_t)((us - base_us) / 1000u);
        int64_t d = t - secs_ms;
        if (d > -1000 && d < 2000)
            return t;
    }
    have = true;
    base_ms = secs_ms;
    base_us = us;
    return secs_ms;
}

int64_t au_mac_local_time(void)
{
    unsigned long secs = 0;
    ReadDateTime(&secs);
    return (int64_t)secs - (int64_t)2082844800;
}
