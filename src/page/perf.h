/* Time counters for the Performance window: where a document's time goes
 * (script, style, layout, paint, the copy to the screen). Portable C99.
 *
 * One AuPerf per document view, cleared when a document begins. Adding
 * to it is a few integer stores, so the counters always run. The view
 * times its own style, layout, paint and script entries with the
 * embedder's millisecond clock (AuDocViewCallbacks.now_ms; without one
 * nothing is timed, only counted); the shell adds the screen copy.
 * Script time includes the style and layout that script forces, which
 * are also counted under their own kinds.
 *
 * au_perf_sample turns the running totals into "the last interval": the
 * Performance window calls it about once a second. Tasks of
 * AU_PERF_SLOW_MS or more are remembered (the last AU_PERF_TASKS of
 * them) with a label. */
#ifndef AURA_PAGE_PERF_H
#define AURA_PAGE_PERF_H

#include "../core/core.h"

typedef enum { AU_PERF_SCRIPT, AU_PERF_STYLE, AU_PERF_LAYOUT, AU_PERF_PAINT, AU_PERF_COPY, AU_PERF_KINDS } AuPerfKind;

#define AU_PERF_TASKS 12
#define AU_PERF_SLOW_MS 50

typedef struct AuPerfTask {
    uint8_t kind;   /* AuPerfKind */
    uint32_t ms;
    uint32_t at_ms; /* the clock when it ended */
    char label[72]; /* a script's URL, or "" */
} AuPerfTask;

typedef struct AuPerf {
    uint32_t ms[AU_PERF_KINDS];    /* since the document began */
    uint32_t count[AU_PERF_KINDS]; /* passes (style, layout, paint, copy) or entries (script) */
    uint32_t kpixels;              /* thousands of pixels painted */
    uint32_t pixel_rem;
    /* The last sample interval. */
    uint32_t last_ms[AU_PERF_KINDS], last_count[AU_PERF_KINDS];
    uint32_t interval_ms; /* its length; 0 before the second sample */
    uint32_t snap_ms[AU_PERF_KINDS], snap_count[AU_PERF_KINDS], snap_at;
    bool sampled;
    /* Slow tasks, a ring: ntasks counts every one seen. */
    AuPerfTask tasks[AU_PERF_TASKS];
    uint32_t ntasks;
} AuPerf;

void au_perf_reset(AuPerf *p);
/* Adds one pass or entry of kind that took ms and ended at now_ms. label
 * may be NULL. */
void au_perf_add(AuPerf *p, AuPerfKind kind, uint32_t ms, const char *label, uint32_t now_ms);
void au_perf_pixels(AuPerf *p, uint32_t pixels);
/* Closes the current interval at now_ms. */
void au_perf_sample(AuPerf *p, uint32_t now_ms);
const char *au_perf_kind_name(int kind);
/* Copies the remembered slow tasks, slowest first, into out (at most
 * max); returns how many. */
int au_perf_slowest(const AuPerf *p, AuPerfTask *out, int max);

#endif
