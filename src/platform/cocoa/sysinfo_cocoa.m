/* AuSysInfo on macOS: sysctl, NSProcessInfo, the main screen and the
 * clock, for the System Status window and the bug report. */
#import "cocoa.h"

#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

static bool sysctl_string(const char *name, char *out, size_t cap)
{
    size_t len = cap;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0 || !len)
        return false;
    out[cap - 1] = 0;
    return true;
}

static uint64_t sysctl_u64(const char *name)
{
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return 0;
    return len == 4 ? (uint64_t)(uint32_t)v : v;
}

/* Free and inactive pages: what the system can give without paging. */
uint32_t au_cocoa_memory_free_kb(void)
{
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n) != KERN_SUCCESS)
        return 0;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    uint64_t kb = ((uint64_t)vm.free_count + vm.inactive_count + vm.purgeable_count) * (uint64_t)page / 1024;
    return kb > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)kb;
}

static void fmt_time(const struct tm *t, char *out, size_t cap)
{
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", t);
}

void au_cocoa_sysinfo(AuSysInfo *s)
{
    memset(s, 0, sizeof *s);
    s->virtual_memory = -1;
    s->enforcer = -1;
    char buf[256];
    if (sysctl_string("hw.model", buf, sizeof buf))
        snprintf(s->model, sizeof s->model, "%s", buf);
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    snprintf(s->os, sizeof s->os, "macOS %ld.%ld.%ld", (long)v.majorVersion, (long)v.minorVersion,
             (long)v.patchVersion);
    if (sysctl_string("machdep.cpu.brand_string", buf, sizeof buf))
        snprintf(s->cpu, sizeof s->cpu, "%s", buf);
    s->cpu_mhz = (uint32_t)(sysctl_u64("hw.cpufrequency") / 1000000);
    s->ram_mb = (uint32_t)(NSProcessInfo.processInfo.physicalMemory / (1024 * 1024));
    NSScreen *scr = NSScreen.mainScreen;
    if (scr) {
        s->screen_w = (int)scr.frame.size.width;
        s->screen_h = (int)scr.frame.size.height;
        s->screen_depth = (int)NSBitsPerPixelFromDepth(scr.depth);
    }
    s->have_memory = true;
    s->heap_free_kb = au_cocoa_memory_free_kb();
    time_t now = time(NULL);
    struct tm utc, local;
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);
    s->have_clock = true;
    fmt_time(&utc, s->utc, sizeof s->utc);
    fmt_time(&utc, s->script_utc, sizeof s->script_utc);
    fmt_time(&local, s->local, sizeof s->local);
    s->zone_seconds = au_posix_utc_offset();
    s->daylight_saving = local.tm_isdst > 0;
}
