/* Time counters for the Performance window (see perf.h). */
#include "perf.h"

#include <stdio.h>
#include <string.h>

void au_perf_reset(AuPerf *p)
{
    memset(p, 0, sizeof *p);
}

void au_perf_add(AuPerf *p, AuPerfKind kind, uint32_t ms, const char *label, uint32_t now_ms)
{
    p->ms[kind] += ms;
    p->count[kind]++;
    if (ms < AU_PERF_SLOW_MS)
        return;
    AuPerfTask *t = &p->tasks[p->ntasks++ % AU_PERF_TASKS];
    t->kind = (uint8_t)kind;
    t->ms = ms;
    t->at_ms = now_ms;
    snprintf(t->label, sizeof t->label, "%s", label ? label : "");
}

void au_perf_pixels(AuPerf *p, uint32_t pixels)
{
    p->pixel_rem += pixels % 1000;
    p->kpixels += pixels / 1000 + p->pixel_rem / 1000;
    p->pixel_rem %= 1000;
}

void au_perf_sample(AuPerf *p, uint32_t now_ms)
{
    if (p->sampled) {
        p->interval_ms = now_ms - p->snap_at;
        for (int i = 0; i < AU_PERF_KINDS; i++) {
            p->last_ms[i] = p->ms[i] - p->snap_ms[i];
            p->last_count[i] = p->count[i] - p->snap_count[i];
        }
    }
    memcpy(p->snap_ms, p->ms, sizeof p->snap_ms);
    memcpy(p->snap_count, p->count, sizeof p->snap_count);
    p->snap_at = now_ms;
    p->sampled = true;
}

const char *au_perf_kind_name(int kind)
{
    static const char *const names[AU_PERF_KINDS] = {"script", "style", "layout", "paint", "screen copy"};
    return kind >= 0 && kind < AU_PERF_KINDS ? names[kind] : "?";
}

int au_perf_slowest(const AuPerf *p, AuPerfTask *out, int max)
{
    int have = p->ntasks < AU_PERF_TASKS ? (int)p->ntasks : AU_PERF_TASKS;
    int n = 0;
    for (int i = 0; i < have; i++) {
        const AuPerfTask *t = &p->tasks[i];
        /* Insertion by duration, longest first. */
        int k = n < max ? n : max;
        while (k > 0 && out[k - 1].ms < t->ms)
            k--;
        if (k >= max)
            continue;
        int last = n < max ? n : max - 1;
        for (int m = last; m > k; m--)
            out[m] = out[m - 1];
        out[k] = *t;
        if (n < max)
            n++;
    }
    return n;
}
