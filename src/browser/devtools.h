/* The developer tools' text, built from the page's models: the
 * Performance report, the console's lines, and (netlog.h) the request
 * log. Portable C99. The shells show this text in windows (textview.h)
 * and `aura-host chrome --do perf|netlog|console` prints it, so the
 * tests check what a user sees. */
#ifndef AURA_BROWSER_DEVTOOLS_H
#define AURA_BROWSER_DEVTOOLS_H

#include "page.h"

/* What only the shell and the network stack know, for the report. Fill
 * with zeros, then what is known. src/net/net_report.c fills the network
 * part (au_net_dev_info). */
typedef struct AuDevHostInfo {
    const char *machine;   /* one line about the machine and system, or NULL */
    bool have_memory;      /* Mac OS 9: the application heap and temporary memory */
    uint32_t heap_free_kb, heap_block_kb, temp_free_kb;
    uint32_t temp_used_kb; /* temporary memory the allocator took because the heap was short */
    bool have_net;
    uint32_t http_cache_kb, http_cache_max_kb, http_cache_entries;
    unsigned long cache_fresh, cache_history, cache_stale, cache_304, cache_missed, cache_stored, cache_disk;
    unsigned long dns_lookups, dns_hits, dns_shared, dns_requests, dns_entries;
    int connections, connections_busy;
    const char *connection_text; /* "example.com:443 x2 (1 busy), ..." or NULL */
} AuDevHostInfo;

/* Closes the page's sample interval and appends the Performance report:
 * time in script, style, layout, paint and screen copy over the last
 * interval and since the document began, pass counts, pixels painted,
 * memory (heap, script, images, glyphs, HTTP cache), connections, load
 * times and the slowest recent tasks. now_ms is the page clock
 * (AuPageEnv.time_ms). Call it about once a second for a live view. */
void au_dev_perf_report(AuPage *p, const AuDevHostInfo *h, uint32_t now_ms, AuBuf *out);

/* ---- Console ----
 * A console line as the Console window shows it. Script errors carry
 * their stack; where a line's text names a source position
 * ("at f (https://host/a.js:12:5)"), source and line are filled. */
typedef struct AuConsoleEntry {
    int level;        /* 0 log, 1 info, 2 warn, 3 error, 4 debug, AU_CONSOLE_INPUT, AU_CONSOLE_RESULT */
    const char *text; /* the whole text; lines after the first are the stack */
    char source[160]; /* "" when unknown */
    int line;         /* 0 when unknown */
} AuConsoleEntry;
bool au_dev_console_entry(const AuPage *p, size_t i, AuConsoleEntry *out);
/* A one-word label for a level: "log", "warn", "error", ">", "<". */
const char *au_dev_console_label(int level);
/* Appends every console line as text, one entry per line with its label
 * (stack lines indented), for Copy and `--do console`. */
void au_dev_console_text(const AuPage *p, AuBuf *out);
/* Runs expr in the page as the Console's input line does: adds the
 * expression and its result (or the error) to the console. Returns false
 * when it threw or could not run. */
bool au_dev_console_eval(AuPage *p, const char *expr);

/* ---- Lines for the text view (textview.h) ----
 * Each replaces the list's contents. */
struct AuTextList;
struct AuNetLog;
struct AuNetRecord;
/* The console: an entry's first line carries its label and, where known,
 * its source; stack lines follow indented. Errors and warnings get their
 * style. */
void au_dev_console_lines(const AuPage *p, struct AuTextList *out);
/* The request log: line i is record i (au_netlog_at). Failed and blocked
 * requests are styled as errors, pending ones dimmed. */
void au_dev_network_lines(const struct AuNetLog *l, struct AuTextList *out);
/* One request's details (au_netlog_detail); "Select a request" for NULL. */
void au_dev_network_detail_lines(const struct AuNetRecord *r, struct AuTextList *out);
/* What the Network window's lower pane shows while no request is
 * selected: the requests that were blocked (au_netlog_blocked), then the
 * hint to select one. */
void au_dev_network_blocked_lines(const struct AuNetLog *l, struct AuTextList *out);
/* A report's text: a line that is not indented and is followed by an
 * indented one becomes a heading. */
void au_dev_report_lines(const char *text, struct AuTextList *out);

#endif
