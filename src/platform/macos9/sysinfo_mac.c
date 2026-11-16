/* The machine's facts for the System Status window and the bug report
 * (browser/sysinfo.h): every Gestalt selector Aura reads for them is
 * here. A selector that fails leaves its field empty, which the reports
 * print as "unknown".
 *
 *   'mach'  machine type          'mnam'  the model's name (a Pascal string)
 *   'cput'  processor type        'pclk', 'bclk'  processor and bus clock, Hz
 *   'ram '  physical memory       'vm  '  virtual memory attributes
 *   'sysv'  system version        'cbon'  CarbonLib version
 *   'appr', 'apvr'  Appearance Manager      'otan', 'otvr'  Open Transport
 *   'Enfr'  the Enforcer extension (github.com/matjam/os9-enforcer) */
#include "mac.h"

#include "browser/sysinfo.h"
#include "clock_mac.h"
#include "js/clock.h"

#include <stdio.h>
#include <string.h>

static bool gestalt(OSType sel, long *out)
{
    long v = 0;
    if (Gestalt(sel, &v) != noErr)
        return false;
    *out = v;
    return true;
}

/* A BCD version word as "1.6" or "9.2.2": 0x0922, 0x0160. */
static void bcd_version(long v, char *out, size_t cap)
{
    int major = (int)((v >> 8) & 0xFF), minor = (int)((v >> 4) & 0xF), fix = (int)(v & 0xF);
    major = (major >> 4) * 10 + (major & 0xF);
    if (fix)
        snprintf(out, cap, "%d.%d.%d", major, minor, fix);
    else
        snprintf(out, cap, "%d.%d", major, minor);
}

static const char *cpu_name(long type, char *buf, size_t cap)
{
    switch (type) {
    case 0x101: return "PowerPC 601";
    case 0x103: return "PowerPC 603";
    case 0x104: return "PowerPC 604";
    case 0x106: return "PowerPC 603e";
    case 0x107: return "PowerPC 603ev";
    case 0x108: return "PowerPC G3 (750)";
    case 0x109: return "PowerPC 604e";
    case 0x10A: return "PowerPC 604ev";
    case 0x10C: return "PowerPC G4 (7400)";
    case 0x110:
    case 0x111: return "PowerPC G4 (7450)";
    default:
        snprintf(buf, cap, "PowerPC (processor type %lx)", type);
        return buf;
    }
}

static void stamp(int64_t t, char out[24])
{
    int64_t day = t / 86400;
    int32_t rem = (int32_t)(t - day * 86400);
    if (rem < 0) {
        rem += 86400;
        day--;
    }
    int y, mo, d, wd;
    au_date_of_day(day, &y, &mo, &d, &wd);
    snprintf(out, 24, "%04d-%02d-%02d %02u:%02u:%02u", y, mo, d, (unsigned)(rem / 3600) % 24u, (unsigned)(rem / 60) % 60u,
             (unsigned)rem % 60u);
}

void mac_sysinfo(AuSysInfo *s)
{
    long v;
    char buf[48];
    memset(s, 0, sizeof *s);
    s->virtual_memory = -1;
    s->enforcer = 0;

    if (gestalt('mach', &v))
        s->machine_type = v;
    /* The name the system shows for the model. New World machines all
     * have machine type 406, so the name is what tells them apart. */
    if (gestalt('mnam', &v) && v) {
        ConstStringPtr name = (ConstStringPtr)v;
        char *u = mac_roman_to_utf8((const char *)name + 1, name[0]);
        snprintf(s->model, sizeof s->model, "%s", u);
        au_free(u);
    }
    if (gestalt('cput', &v))
        snprintf(s->cpu, sizeof s->cpu, "%s", cpu_name(v, buf, sizeof buf));
    if (gestalt('pclk', &v))
        s->cpu_mhz = (uint32_t)(((unsigned long)v + 500000) / 1000000);
    if (gestalt('bclk', &v))
        s->bus_mhz = (uint32_t)(((unsigned long)v + 500000) / 1000000);
    if (gestalt('ram ', &v))
        s->ram_mb = (uint32_t)((unsigned long)v / (1024 * 1024));
    if (gestalt('vm  ', &v))
        s->virtual_memory = (v & 1) ? 1 : 0;
    if (gestalt('sysv', &v)) {
        char ver[16];
        bcd_version(v, ver, sizeof ver);
        snprintf(s->os, sizeof s->os, "Mac OS %s", ver);
    }
    if (gestalt('cbon', &v))
        bcd_version(v, s->carbonlib, sizeof s->carbonlib);
    else
        snprintf(s->carbonlib, sizeof s->carbonlib, "not installed");
    if (gestalt('apvr', &v))
        bcd_version(v, s->appearance, sizeof s->appearance);
    else if (gestalt('appr', &v))
        snprintf(s->appearance, sizeof s->appearance, "installed");
    else
        snprintf(s->appearance, sizeof s->appearance, "not installed");
    /* Open Transport's version is a NumVersion: BCD major, then minor and
     * bug fix nibbles. */
    if (gestalt('otvr', &v) && v)
        bcd_version((long)(((unsigned long)v >> 16) & 0xFFFF), s->open_transport, sizeof s->open_transport);
    else if (gestalt('otan', &v) && v)
        snprintf(s->open_transport, sizeof s->open_transport, "installed");
    else
        snprintf(s->open_transport, sizeof s->open_transport, "not installed");

    Rect sb = qd.screenBits.bounds;
    GDHandle gd = GetMainDevice();
    s->screen_w = sb.right - sb.left;
    s->screen_h = sb.bottom - sb.top;
    s->screen_depth = gd ? (**(**gd).gdPMap).pixelSize : 0;

    /* Enforcer's extension answers Gestalt 'Enfr': a small number is a
     * status code (loaded, not protecting), anything else its state. */
    if (gestalt('Enfr', &v)) {
        s->enforcer = 1;
        if ((unsigned long)v >= 0x1000)
            snprintf(s->enforcer_note, sizeof s->enforcer_note, "extension running");
        else
            snprintf(s->enforcer_note, sizeof s->enforcer_note, "extension loaded, status %ld", v);
    }

    /* Aura's own memory. */
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    s->have_memory = true;
    if (GetCurrentProcess(&psn) == noErr && GetProcessInformation(&psn, &info) == noErr)
        s->partition_kb = (uint32_t)(info.processSize / 1024);
    s->heap_free_kb = (uint32_t)(FreeMem() / 1024);
    s->heap_block_kb = (uint32_t)(MaxBlock() / 1024);
    s->temp_free_kb = (uint32_t)((unsigned long)TempFreeMem() / 1024);

    /* The clock (see clock_mac.h): UTC is the Mac's clock minus the zone
     * offset of the Date & Time control panel. */
    s->have_clock = true;
    stamp(au_mac_wall_time(), s->utc);
    stamp(au_js_clock_now_ms() / 1000, s->script_utc);
    stamp(au_mac_local_time(), s->local);
    s->zone_seconds = au_mac_gmt_offset();
    s->daylight_saving = au_mac_daylight_saving();
    s->script_zone_differs = au_js_clock_utc_offset_s(au_js_clock_now_ms()) != s->zone_seconds;
}
