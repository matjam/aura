/* The page benchmark: about:bench?MANIFEST. Portable C99.
 *
 * Typing about:bench?URL in a window's address field fetches the
 * manifest at URL and loads each page it lists in the active tab, one
 * after another, the number of times it says. For each load it records
 * what the Performance window shows: first paint and load time, the time
 * in script, style, layout, paint and screen copy, the work counted, and
 * memory. When every load is done it posts the results as tab-separated
 * text to the manifest's post URL and shows them in the tab.
 *
 * The manifest is plain text, one item per line:
 *   # a comment
 *   repeat N      loads of each page (default 3, at most 50)
 *   settle MS     time after a page has loaded before it is measured,
 *                 so its timers run (default 1000)
 *   timeout MS    a load not finished by then is recorded as a timeout
 *                 (default 120000)
 *   post URL      where the results go (default "result" beside the
 *                 manifest; "none" posts nothing)
 *   URL           a page to load, relative to the manifest
 *
 * The pages load in rounds (every page once, then every page again), so
 * the first round's numbers include whatever a cold start costs. The
 * results start with "# aura-bench 1", "# build IDENTITY" and
 * "# manifest URL" lines, then a line of column names, then a row per
 * load. docs/performance.md says how tools/bench.sh runs it in the guest
 * and on the host. */
#ifndef AURA_BROWSER_BENCH_H
#define AURA_BROWSER_BENCH_H

#include "browser.h"

#define AU_BENCH_PREFIX "about:bench?"

typedef struct AuBench AuBench;

/* Starts the benchmark of manifest_url in w's active tab. NULL when w has
 * no tab. */
AuBench *au_bench_start(AuBrowserWindow *w, const char *manifest_url);
/* Cancels a run under way. NULL is fine. */
void au_bench_free(AuBench *b);
/* Call from the window's idle: moves the run along. Returns false once
 * the run is over (the caller may then free it). */
bool au_bench_idle(AuBench *b);
/* The results so far (the header and a row per finished load). */
const char *au_bench_results(const AuBench *b);
/* True once the run is over and its results were posted (or there was
 * nothing to post to); false while it runs or when it failed. */
bool au_bench_ok(const AuBench *b);

#endif
