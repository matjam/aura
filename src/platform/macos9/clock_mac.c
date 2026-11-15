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

int32_t au_mac_gmt_offset(void)
{
    AuMacLocation loc;
    loc.latitude = loc.longitude = 0;
    loc.gmt_delta = 0;
    ReadLocation(&loc);
    /* The low 24 bits are the signed offset in seconds; the Date & Time
     * control panel keeps the daylight saving hour in it while daylight
     * saving is on. */
    int32_t d = loc.gmt_delta & 0x00FFFFFF;
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
