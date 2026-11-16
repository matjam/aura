/* What Aura knows about the machine it runs on and about itself: the
 * facts the System Status window shows and the bug report repeats, so
 * that both come from one place. Portable C99: the shell fills an
 * AuSysInfo (on Mac OS 9 from Gestalt, src/platform/macos9/sysinfo_mac.c)
 * and this file turns it into text. */
#ifndef AURA_BROWSER_SYSINFO_H
#define AURA_BROWSER_SYSINFO_H

#include "browser.h"
#include "devtools.h"

/* Fill with zeros, then what is known. An empty string or a zero prints
 * as "unknown". */
typedef struct AuSysInfo {
    /* The machine. */
    char model[64];     /* "Power Macintosh G4 (AGP Graphics)"; "" when the type is not in the table */
    long machine_type;  /* the Gestalt machine type; 0 when not known */
    char cpu[48];       /* "PowerPC G4 (7400)" */
    uint32_t cpu_mhz, bus_mhz;
    uint32_t ram_mb;    /* physical memory */
    int virtual_memory; /* 1 on, 0 off, -1 not known */
    char os[40];        /* "Mac OS 9.2.2" */
    char carbonlib[24], appearance[24], open_transport[24]; /* versions; "not installed" when absent */
    int screen_w, screen_h, screen_depth;
    int enforcer;       /* 1 installed, 0 not, -1 not known */
    char enforcer_note[64]; /* what its Gestalt value says; "" for none */
    /* Aura's memory. */
    bool have_memory;
    uint32_t partition_kb, heap_free_kb, heap_block_kb, temp_free_kb;
    /* The clock, as "YYYY-MM-DD HH:MM:SS". */
    bool have_clock;
    char utc[24];        /* UTC as Aura computes it: the Mac's clock minus the zone offset */
    char script_utc[24]; /* what script's Date.now() gives */
    char local[24];      /* the Mac's clock */
    int32_t zone_seconds; /* east of UTC, daylight saving included */
    bool daylight_saving;
    bool script_zone_differs; /* script's zone offset is not zone_seconds */
} AuSysInfo;

/* "0.3.0 (1a2b3c4)": AU_VERSION and, when the build recorded it, the
 * short commit ("+" after it when the tree had uncommitted changes). */
const char *au_build_identity(void);
/* The date of that commit, "2026-11-15"; "unknown" when not recorded. */
const char *au_build_date(void);

/* The machine and system in one line, for the Performance report. */
void au_sysinfo_line(const AuSysInfo *s, char *out, size_t cap);
/* The sections, each as "label value" lines (au_report_kv) without a
 * title: the caller writes the heading (plain text or Markdown). */
void au_sysinfo_machine(const AuSysInfo *s, AuBuf *out);
void au_sysinfo_clock(const AuSysInfo *s, AuBuf *out);
/* Aura's state: memory, windows and tabs, kept documents, the script
 * heap of every page (named by window and tab number, never by address)
 * and the caches. s and h may be NULL. */
void au_sysinfo_state(const AuSysInfo *s, AuBrowser *b, const AuDevHostInfo *h, AuBuf *out);

/* The System Status report: a summary, Aura, the machine, the clock,
 * Aura's state, fonts, then shell_sections (text the shell formatted the
 * same way: the network stack, blocking; NULL for none) and the paint
 * benchmark when one ran. */
void au_sysinfo_report(const AuSysInfo *s, AuBrowser *b, const AuDevHostInfo *h, const char *shell_sections,
                       const AuPaintBench *bench, AuBuf *out);

#endif
