/* Clocks for the network stack on Mac OS 9.
 *
 * The system clock counts local seconds from 1904. The offset from UTC is
 * the one set in the Map (or Date & Time) control panel, read with
 * ReadLocation. If it was never set the offset is 0 and times are taken
 * as UTC, which can make certificate dates and cookie expiry wrong by the
 * time zone's offset. */
#ifndef AURA_CLOCK_MAC_H
#define AURA_CLOCK_MAC_H

#include <Multiverse.h>

#include <stdbool.h>
#include <stdint.h>

/* Milliseconds since startup; wraps after 49 days. */
uint32_t au_mac_clock_ms(void);
/* Microseconds since startup, for timing short work; wraps after 71
 * minutes, so use differences. */
uint32_t au_mac_clock_us(void);
/* Seconds east of UTC, from the Map control panel (0 if unset). */
int32_t au_mac_gmt_offset(void);
/* Seconds since 1970-01-01 00:00 UTC. */
int64_t au_mac_wall_time(void);
/* The same in milliseconds, for script's Date: the system clock's
 * seconds with the milliseconds of the time since startup. */
int64_t au_mac_wall_ms(void);
/* The system clock as it is set: local seconds counted from 1970. */
int64_t au_mac_local_time(void);
/* Whether the Date & Time control panel has daylight saving on (the
 * hour is then already in au_mac_gmt_offset). */
bool au_mac_daylight_saving(void);

#endif
