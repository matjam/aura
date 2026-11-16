/* The developer tools' text, built from the page's models: the
 * Performance report, the console's lines, the timing test and the paint
 * benchmark, and (netlog.h) the request log. Portable C99. The shells
 * show this text in windows (textview.h) and `aura-host chrome --do
 * perf|netlog|console|status|bugreport` prints it, so the tests check
 * what a user sees. sysinfo.h has the System Status report and
 * bugreport.h the bug report.
 *
 * Every report follows one layout, so that the windows read alike:
 *   - a section is a title at the left margin, then its lines indented
 *     by two spaces, then a blank line;
 *   - a line is a label padded to AU_REPORT_LABEL cells, then its value;
 *     tables put numbers right-aligned under a line of column titles;
 *   - counts carry thousands separators (au_put_count), sizes are B, KB
 *     or MB (au_put_size), times ms or s (au_put_ms). */
#ifndef AURA_BROWSER_DEVTOOLS_H
#define AURA_BROWSER_DEVTOOLS_H

#include "page.h"

/* ---- Where the windows were left ----
 * The develop.windows preference (AuPrefs.dev_windows): "name=x,y,w,h"
 * items separated by spaces. au_dev_window_get reads one window's place
 * (false when it has none, or a size under 100 pixels);
 * au_dev_window_set writes the preference's new text to out. */
bool au_dev_window_get(const char *pref, const char *name, int rect[4]);
void au_dev_window_set(const char *pref, const char *name, const int rect[4], AuBuf *out);

/* ---- The timing test (Develop > Run Timing Test) ----
 * The shell scrolls and repaints the active page and fills this; the
 * Performance report shows it as a table. Times are microseconds over
 * all steps of a row. */
typedef struct AuTimedSteps {
    uint32_t steps;    /* 0: the row was not run */
    uint32_t total_us; /* event to pixels on screen */
    uint32_t move_us;  /* moving pixels already painted */
    uint32_t paint_us; /* painting what was exposed */
    uint32_t copy_us;  /* copying to the screen */
    uint32_t areas;    /* rectangles painted */
} AuTimedSteps;

typedef struct AuTimingTest {
    bool ran;
    uint32_t at_ms;        /* the page clock when it ran */
    AuTimedSteps typing;   /* 16 keys in the focused text field */
    uint32_t key_restyles, key_elements, key_layouts;
    AuTimedSteps small;    /* ten 32-pixel scroll steps over content not painted before */
    AuTimedSteps again;    /* the same steps again */
    AuTimedSteps page;     /* five page downs */
    AuTimedSteps full;     /* three repaints of the whole page */
    uint32_t glyph_hits, glyph_misses, glyph_evictions; /* in the scrolls and repaints */
} AuTimingTest;
/* Appends the "Timing test" section; nothing when it has not run. */
void au_dev_timing_report(const AuTimingTest *t, AuBuf *out);

/* ---- The paint benchmark (Develop > Tests > Paint Benchmark) ----
 * Times in tenths of a millisecond per run. */
typedef struct AuPaintBench {
    bool ran;
    int width, height;        /* the area painted */
    uint32_t scene_warm, scene_cold; /* the test scene with cached glyphs, and with the glyph cache emptied */
    uint32_t fill;            /* one alpha fill of the area */
    uint32_t chrome;          /* the toolbar and tabs */
} AuPaintBench;
/* Appends the "Paint benchmark" section; nothing when it has not run. */
void au_dev_bench_report(const AuPaintBench *b, AuBuf *out);

/* What only the shell and the network stack know, for the report. Fill
 * with zeros, then what is known. src/net/net_report.c fills the network
 * part (au_net_dev_info). */
typedef struct AuDevHostInfo {
    const char *machine;   /* one line about the machine and system (au_sysinfo_line), or NULL */
    const char *build;     /* Aura's version and commit (au_build_identity), or NULL */
    bool have_memory;      /* Mac OS 9: the application heap and temporary memory */
    uint32_t partition_kb; /* the application heap's size; 0 when not known */
    uint32_t heap_free_kb, heap_block_kb, temp_free_kb;
    bool have_net;
    uint32_t http_cache_kb, http_cache_max_kb, http_cache_entries;
    unsigned long cache_fresh, cache_history, cache_stale, cache_304, cache_missed, cache_stored, cache_disk;
    unsigned long dns_lookups, dns_hits, dns_shared, dns_requests, dns_entries;
    int connections, connections_busy;
    const char *connection_text; /* "example.com:443 x2 (1 busy), ..." or NULL */
    const AuTimingTest *timing;  /* the last timing test of this page, or NULL */
} AuDevHostInfo;

/* Closes the page's sample interval and appends the Performance report:
 * a summary (load time, requests, how busy the last interval was, free
 * memory), the time in script, style, layout, paint and screen copy over
 * the last interval and since the document began with a bar each, the
 * work counted, memory with bars, the network, the slowest recent tasks,
 * the timing test when one ran, and a legend. now_ms is the page clock
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
/* Which entries the Console window lists. Input and results are always
 * listed. */
enum { AU_CONSOLE_SHOW_ERRORS = 1, AU_CONSOLE_SHOW_WARNINGS = 2, AU_CONSOLE_SHOW_LOGS = 4, AU_CONSOLE_SHOW_ALL = 7 };
typedef struct AuConsoleFilter {
    unsigned show;    /* AU_CONSOLE_SHOW_* bits */
    const char *text; /* entries whose text contains it (ASCII case-insensitive); NULL or "" for all */
} AuConsoleFilter;
bool au_dev_console_shows(const AuConsoleFilter *f, const AuConsoleEntry *e);
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
struct AuNetView;
/* The console: an entry's first line carries its label and, where known,
 * its source; stack lines follow indented. Errors and warnings get their
 * style. The console keeps lines across loads: when it holds lines of
 * earlier pages, a heading line with the page's address marks where the
 * current page's lines begin (au_page_console_current). f NULL lists
 * everything. Returns how many entries the filter left out. */
uint32_t au_dev_console_lines(const AuPage *p, const AuConsoleFilter *f, struct AuTextList *out);
/* The request log: line i is record v->rows[i] (au_netview_build first),
 * its URL cut in the middle to fit cells cells (0: whole). Failed and
 * blocked requests are styled as errors, pending ones dimmed. */
void au_dev_network_lines(const struct AuNetLog *l, const struct AuNetView *v, int cells, struct AuTextList *out);
/* One request's details (au_netlog_detail). */
void au_dev_network_detail_lines(const struct AuNetRecord *r, struct AuTextList *out);
/* What the Network window's lower pane shows while no request is
 * selected: the summary (au_netlog_summary), then the hint to select a
 * request. */
void au_dev_network_summary_lines(const struct AuNetLog *l, const char *connections, struct AuTextList *out);
/* A report's text: a line that is not indented and is followed by an
 * indented one becomes a heading. */
void au_dev_report_lines(const char *text, struct AuTextList *out);
/* Markdown (the bug report): "#" lines become headings; fence lines and
 * the <details> and <summary> tags are dimmed. */
void au_dev_markdown_lines(const char *text, struct AuTextList *out);

#endif
