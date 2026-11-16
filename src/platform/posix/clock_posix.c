/* Clocks and the time zone over POSIX (see posix.h). */
#include "posix.h"

#include <time.h>

uint32_t au_posix_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

uint32_t au_posix_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

int64_t au_posix_wall_time(void)
{
    return (int64_t)time(NULL);
}

int32_t au_posix_utc_offset(void)
{
    time_t t = time(NULL);
    struct tm lt, ut;
    if (!localtime_r(&t, &lt) || !gmtime_r(&t, &ut))
        return 0;
    /* The difference of the two broken-down times, in seconds. */
    long d = (long)(lt.tm_hour - ut.tm_hour) * 3600 + (long)(lt.tm_min - ut.tm_min) * 60;
    int day = (lt.tm_year != ut.tm_year) ? (lt.tm_year > ut.tm_year ? 1 : -1) : (lt.tm_yday - ut.tm_yday);
    return (int32_t)(d + (long)day * 86400);
}

uint32_t au_platform_time_ms(void)
{
    return au_posix_ms();
}
