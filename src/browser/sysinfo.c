/* The System Status report and the build identity (see sysinfo.h). */
#include "sysinfo.h"
#include "credits.h"
#include "netlog.h"

#include <stdio.h>
#include <string.h>

/* au_build.h is written by tools/buildinfo.cmake at build time: the
 * commit and its date. A build without it (another build system) says
 * so by defining nothing. */
#if defined(__has_include)
#if __has_include("au_build.h")
#include "au_build.h"
#endif
#endif
#ifndef AU_BUILD_COMMIT
#define AU_BUILD_COMMIT ""
#endif
#ifndef AU_BUILD_DATE
#define AU_BUILD_DATE ""
#endif

const char *au_build_identity(void)
{
    return AU_BUILD_COMMIT[0] ? AU_VERSION " (" AU_BUILD_COMMIT ")" : AU_VERSION;
}

const char *au_build_date(void)
{
    return AU_BUILD_DATE[0] ? AU_BUILD_DATE : "unknown";
}

static const char *or_unknown(const char *s)
{
    return s && s[0] ? s : "unknown";
}

static void put_size_kb(AuBuf *out, uint32_t kb)
{
    if (kb >= 4u * 1024 * 1024)
        au_buf_printf(out, "%lu MB", (unsigned long)(kb / 1024));
    else
        au_put_size(out, kb * 1024u);
}

static const char *size_kb(char *b, size_t cap, uint32_t kb)
{
    AuBuf t;
    au_buf_init(&t);
    put_size_kb(&t, kb);
    snprintf(b, cap, "%s", au_buf_cstr(&t));
    au_buf_free(&t);
    return b;
}

void au_sysinfo_line(const AuSysInfo *s, char *out, size_t cap)
{
    char mhz[24] = "", ram[24] = "", screen[40] = "";
    if (s->cpu_mhz)
        snprintf(mhz, sizeof mhz, " %lu MHz", (unsigned long)s->cpu_mhz);
    if (s->ram_mb)
        snprintf(ram, sizeof ram, ", %lu MB", (unsigned long)s->ram_mb);
    if (s->screen_w)
        snprintf(screen, sizeof screen, ", %dx%dx%d", s->screen_w, s->screen_h, s->screen_depth);
    snprintf(out, cap, "%s%s%s%s%s, %s%s", s->model[0] ? s->model : "", s->model[0] ? ", " : "", or_unknown(s->cpu), mhz,
             ram, or_unknown(s->os), screen);
}

void au_sysinfo_machine(const AuSysInfo *s, AuBuf *out)
{
    if (s->model[0] && s->machine_type)
        au_report_kv(out, "Model", "%s (machine type %ld)", s->model, s->machine_type);
    else if (s->machine_type)
        au_report_kv(out, "Model", "unknown (machine type %ld)", s->machine_type);
    else
        au_report_kv(out, "Model", "%s", or_unknown(s->model));
    if (s->cpu_mhz && s->bus_mhz)
        au_report_kv(out, "Processor", "%s, %lu MHz (bus %lu MHz)", or_unknown(s->cpu), (unsigned long)s->cpu_mhz,
                     (unsigned long)s->bus_mhz);
    else if (s->cpu_mhz)
        au_report_kv(out, "Processor", "%s, %lu MHz", or_unknown(s->cpu), (unsigned long)s->cpu_mhz);
    else
        au_report_kv(out, "Processor", "%s, speed unknown", or_unknown(s->cpu));
    if (s->ram_mb)
        au_report_kv(out, "Memory installed", "%lu MB", (unsigned long)s->ram_mb);
    else
        au_report_kv(out, "Memory installed", "unknown");
    au_report_kv(out, "Virtual memory", "%s", s->virtual_memory > 0 ? "on" : s->virtual_memory == 0 ? "off" : "unknown");
    au_report_kv(out, "System", "%s", or_unknown(s->os));
    au_report_kv(out, "CarbonLib", "%s", or_unknown(s->carbonlib));
    au_report_kv(out, "Appearance Manager", "%s", or_unknown(s->appearance));
    au_report_kv(out, "Open Transport", "%s", or_unknown(s->open_transport));
    if (s->screen_w)
        au_report_kv(out, "Screen", "%d x %d pixels, %d bits per pixel", s->screen_w, s->screen_h, s->screen_depth);
    else
        au_report_kv(out, "Screen", "unknown");
    if (s->enforcer > 0)
        au_report_kv(out, "Enforcer", "installed%s%s", s->enforcer_note[0] ? ", " : "", s->enforcer_note);
    else
        au_report_kv(out, "Enforcer", "%s", s->enforcer == 0 ? "not installed" : "unknown");
}

void au_sysinfo_clock(const AuSysInfo *s, AuBuf *out)
{
    if (!s->have_clock) {
        au_report_kv(out, "UTC as Aura computes", "unknown");
        return;
    }
    int32_t a = s->zone_seconds < 0 ? -s->zone_seconds : s->zone_seconds;
    au_report_kv(out, "UTC as Aura computes", "%s", s->utc);
    au_report_kv(out, "UTC in script", "%s%s", s->script_utc, s->script_zone_differs ? " (script uses another zone)" : "");
    au_report_kv(out, "The Mac's clock", "%s", s->local);
    au_report_kv(out, "Time zone", "UTC%c%d:%02d", s->zone_seconds < 0 ? '-' : '+', (int)(a / 3600), (int)(a % 3600 / 60));
    au_report_kv(out, "Daylight saving", "%s", s->daylight_saving ? "on" : "off");
}

void au_sysinfo_state(const AuSysInfo *s, AuBrowser *b, const AuDevHostInfo *h, AuBuf *out)
{
    char x[24], y[24];
    if (s && s->have_memory) {
        au_report_kv(out, "Memory partition", "%s", size_kb(x, sizeof x, s->partition_kb));
        au_report_kv(out, "Free heap", "%s (largest block %s)", size_kb(x, sizeof x, s->heap_free_kb),
                     size_kb(y, sizeof y, s->heap_block_kb));
        au_report_kv(out, "Temporary memory", "%s free", size_kb(x, sizeof x, s->temp_free_kb));
    } else {
        au_report_kv(out, "Memory partition", "unknown");
    }
    int tabs = 0;
    for (int i = 0; b && i < b->nwindows; i++)
        tabs += b->windows[i]->ntabs;
    au_report_kv(out, "Windows and tabs", "%d window%s, %d tab%s", b ? b->nwindows : 0, b && b->nwindows == 1 ? "" : "s",
                 tabs, tabs == 1 ? "" : "s");
    if (b) {
        size_t bytes = 0, n = au_browser_kept_documents(b, &bytes);
        au_report_kv(out, "Kept documents", "%lu for Back and Forward, about %s", (unsigned long)n,
                     size_kb(x, sizeof x, (uint32_t)(bytes / 1024)));
    }
    /* Each page's script heap, by window and tab number. */
    for (int i = 0; b && i < b->nwindows; i++) {
        AuBrowserWindow *w = b->windows[i];
        for (int k = 0; k < w->ntabs; k++) {
            size_t used = 0, limit = 0;
            char label[40];
            au_page_script_memory(w->tabs[k]->page, &used, &limit);
            snprintf(label, sizeof label, "Script heap, tab %d.%d", i + 1, k + 1);
            if (limit)
                au_report_kv(out, label, "%s of %s%s", size_kb(x, sizeof x, (uint32_t)((used + 1023) / 1024)),
                             size_kb(y, sizeof y, (uint32_t)(limit / 1024)), k == w->active ? "   (the active tab)" : "");
            else
                au_report_kv(out, label, "no script runtime%s", k == w->active ? "   (the active tab)" : "");
        }
    }
    if (h && h->have_net)
        au_report_kv(out, "HTTP memory cache", "%s of %s, %lu entries", size_kb(x, sizeof x, h->http_cache_kb),
                     size_kb(y, sizeof y, h->http_cache_max_kb), (unsigned long)h->http_cache_entries);
    if (b) {
        au_report_kv(out, "Disk cache limit", "%s", size_kb(x, sizeof x, (uint32_t)b->prefs.disk_cache_kb));
        AuGlyphCacheStats gs;
        au_glyph_cache_stats(&gs);
        au_report_kv(out, "Glyph cache", "%s, %lu glyphs", size_kb(x, sizeof x, (uint32_t)(gs.bytes / 1024)),
                     (unsigned long)gs.entries);
    }
}

void au_sysinfo_report(const AuSysInfo *s, AuBrowser *b, const AuDevHostInfo *h, const char *shell_sections,
                       const AuPaintBench *bench, AuBuf *out)
{
    char line[256], x[24], y[24];
    AuSysInfo none;
    if (!s) {
        memset(&none, 0, sizeof none);
        none.virtual_memory = none.enforcer = -1;
        s = &none;
    }
    au_buf_puts(out, "System Status\n");
    /* The summary: what a person wants first. */
    au_report_kv(out, "Aura", "%s, built %s", au_build_identity(), au_build_date());
    au_sysinfo_line(s, line, sizeof line);
    au_report_kv(out, "Machine", "%s", line);
    if (s->have_memory)
        au_report_kv(out, "Free memory", "%s of %s", size_kb(x, sizeof x, s->heap_free_kb),
                     size_kb(y, sizeof y, s->partition_kb));
    if (s->have_clock)
        au_report_kv(out, "Clock (UTC)", "%s", s->utc);

    au_report_heading(out, "Machine");
    au_sysinfo_machine(s, out);

    au_report_heading(out, "Clock");
    au_sysinfo_clock(s, out);
    au_buf_puts(out, "  A wrong zone or daylight saving setting makes certificates and cookies fail.\n");

    au_report_heading(out, "Aura");
    au_report_kv(out, "Version", "%s", au_build_identity());
    au_report_kv(out, "Built from", "the commit of %s", au_build_date());
    if (b)
        au_report_kv(out, "User agent", "%s", au_prefs_user_agent(&b->prefs));
    au_sysinfo_state(s, b, h, out);

    if (b && b->fonts) {
        AuFontSetStats fst;
        au_fontset_stats(b->fonts, &fst);
        au_report_heading(out, "Fonts");
        au_report_kv(out, "Faces", "%lu found, %lu loaded (%s)", (unsigned long)au_fontset_count(b->fonts),
                     (unsigned long)fst.loaded, size_kb(x, sizeof x, (uint32_t)(fst.loaded_bytes / 1024)));
    }
    if (shell_sections && shell_sections[0]) {
        /* The shell's sections follow the same layout; the first is
         * written to continue the Fonts section when it starts indented. */
        if (shell_sections[0] != ' ')
            au_buf_putc(out, '\n');
        au_buf_puts(out, shell_sections);
        if (out->len && out->data[out->len - 1] != '\n')
            au_buf_putc(out, '\n');
    }
    au_dev_bench_report(bench, out);
}
