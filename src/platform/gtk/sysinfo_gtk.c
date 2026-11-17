/* AuSysInfo on Linux: uname, /etc/os-release, /proc/cpuinfo and
 * /proc/meminfo, and the clock. The System Status window and the bug
 * report read it once they exist in this port. */
#include "gtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

/* The value of "key<sep>" on a line of a text file, trimmed and unquoted. */
static bool file_value(const char *path, const char *key, char sep, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;
    char line[512];
    size_t klen = strlen(key);
    bool found = false;
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, key, klen))
            continue;
        char *p = line + klen;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != sep)
            continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '"')
            p++;
        size_t n = strcspn(p, "\"\n");
        snprintf(out, cap, "%.*s", (int)n, p);
        found = true;
        break;
    }
    fclose(fp);
    return found;
}

static uint32_t meminfo_kb(const char *key)
{
    char v[64];
    if (!file_value("/proc/meminfo", key, ':', v, sizeof v))
        return 0;
    return (uint32_t)strtoul(v, NULL, 10);
}

uint32_t au_gtk_memory_free_kb(void)
{
    return meminfo_kb("MemAvailable");
}

static void fmt_time(const struct tm *t, char *out, size_t cap)
{
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", t);
}

void au_gtk_sysinfo(AuSysInfo *s)
{
    memset(s, 0, sizeof *s);
    s->virtual_memory = -1;
    s->enforcer = -1;
    struct utsname u;
    if (uname(&u) == 0)
        g_strlcpy(s->model, u.machine, sizeof s->model);
    char pretty[128];
    if (file_value("/etc/os-release", "PRETTY_NAME", '=', pretty, sizeof pretty) ||
        file_value("/usr/lib/os-release", "PRETTY_NAME", '=', pretty, sizeof pretty))
        g_strlcpy(s->os, pretty, sizeof s->os);
    else if (uname(&u) == 0)
        g_snprintf(s->os, sizeof s->os, "%s %s", u.sysname, u.release);
    char cpu[128];
    if (file_value("/proc/cpuinfo", "model name", ':', cpu, sizeof cpu))
        g_strlcpy(s->cpu, cpu, sizeof s->cpu);
    char mhz[32];
    if (file_value("/proc/cpuinfo", "cpu MHz", ':', mhz, sizeof mhz))
        s->cpu_mhz = (uint32_t)strtoul(mhz, NULL, 10);
    s->ram_mb = meminfo_kb("MemTotal") / 1024;
    GdkDisplay *d = gdk_display_get_default();
    if (d) {
        GListModel *mons = gdk_display_get_monitors(d);
        GdkMonitor *m = mons ? g_list_model_get_item(mons, 0) : NULL;
        if (m) {
            GdkRectangle r;
            gdk_monitor_get_geometry(m, &r);
            s->screen_w = r.width;
            s->screen_h = r.height;
            s->screen_depth = 32;
            g_object_unref(m);
        }
    }
    s->have_memory = true;
    s->heap_free_kb = au_gtk_memory_free_kb();
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
