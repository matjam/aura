/* The developer tools' text (see devtools.h). */
#include "devtools.h"
#include "netlog.h"
#include "textview.h"

#include "../image/cache.h"
#include "../page/page.h" /* AuDocViewStats */
#include "../page/perf.h"

#include <stdio.h>
#include <string.h>

/* ---- Formatting ---- */

/* Numbers as short strings for table cells. */
static const char *s_count(char *b, size_t cap, unsigned long n)
{
    AuBuf t;
    au_buf_init(&t);
    au_put_count(&t, n);
    snprintf(b, cap, "%s", au_buf_cstr(&t));
    au_buf_free(&t);
    return b;
}

static const char *s_size(char *b, size_t cap, uint32_t bytes)
{
    AuBuf t;
    au_buf_init(&t);
    au_put_size(&t, bytes);
    snprintf(b, cap, "%s", au_buf_cstr(&t));
    au_buf_free(&t);
    return b;
}

static const char *s_kb(char *b, size_t cap, uint32_t kb)
{
    /* Kilobytes to bytes without overflow: 4 GB and more is shown in MB. */
    if (kb >= 4u * 1024 * 1024) {
        snprintf(b, cap, "%lu MB", (unsigned long)(kb / 1024));
        return b;
    }
    return s_size(b, cap, kb * 1024u);
}

static const char *s_ms(char *b, size_t cap, uint32_t ms)
{
    AuBuf t;
    au_buf_init(&t);
    au_put_ms(&t, ms);
    snprintf(b, cap, "%s", au_buf_cstr(&t));
    au_buf_free(&t);
    return b;
}

static unsigned percent(uint32_t part, uint32_t whole)
{
    if (!whole)
        return 0;
    double p = (double)part * 100.0 / (double)whole + 0.5;
    return p > 100.0 ? 100u : (unsigned)p;
}

/* ---- Where the windows were left ---- */

bool au_dev_window_get(const char *pref, const char *name, int rect[4])
{
    size_t nl = strlen(name);
    const char *s = pref ? pref : "";
    while (*s) {
        while (*s == ' ')
            s++;
        size_t n = strcspn(s, " ");
        if (n > nl && !strncmp(s, name, nl) && s[nl] == '=') {
            int v[4] = {0, 0, 0, 0};
            if (sscanf(s + nl + 1, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4 && v[2] >= 100 && v[3] >= 100 &&
                v[2] <= 8192 && v[3] <= 8192 && v[0] > -8192 && v[0] < 8192 && v[1] > -8192 && v[1] < 8192) {
                memcpy(rect, v, sizeof v);
                return true;
            }
            return false;
        }
        s += n;
    }
    return false;
}

void au_dev_window_set(const char *pref, const char *name, const int rect[4], AuBuf *out)
{
    size_t nl = strlen(name);
    const char *s = pref ? pref : "";
    while (*s) {
        while (*s == ' ')
            s++;
        size_t n = strcspn(s, " ");
        if (n && !(n > nl && !strncmp(s, name, nl) && s[nl] == '=')) {
            if (out->len)
                au_buf_putc(out, ' ');
            au_buf_append(out, s, n);
        }
        s += n;
    }
    if (out->len)
        au_buf_putc(out, ' ');
    au_buf_printf(out, "%s=%d,%d,%d,%d", name, rect[0], rect[1], rect[2], rect[3]);
}

/* ---- The timing test and the paint benchmark ---- */

static void timed_row(AuBuf *out, const char *name, const AuTimedSteps *s)
{
    if (!s->steps)
        return;
    uint32_t n = s->steps;
    /* Tenths of a millisecond per step. */
    uint32_t total = s->total_us / 100 / n, move = s->move_us / 100 / n;
    uint32_t paint = (s->paint_us > s->move_us ? s->paint_us - s->move_us : 0) / 100 / n, copy = s->copy_us / 100 / n;
    au_buf_printf(out, "  %-*s %5lu %6lu.%lu %6lu.%lu %6lu.%lu %6lu.%lu %7lu\n", AU_REPORT_LABEL, name, (unsigned long)n,
                  (unsigned long)(total / 10), (unsigned long)(total % 10), (unsigned long)(move / 10),
                  (unsigned long)(move % 10), (unsigned long)(paint / 10), (unsigned long)(paint % 10),
                  (unsigned long)(copy / 10), (unsigned long)(copy % 10), (unsigned long)((s->areas + n / 2) / n));
}

void au_dev_timing_report(const AuTimingTest *t, AuBuf *out)
{
    if (!t || !t->ran)
        return;
    char a[24], b[24], c[24];
    au_report_heading(out, "Timing test (milliseconds per step)");
    au_buf_printf(out, "  %-*s %5s %8s %8s %8s %8s %7s\n", AU_REPORT_LABEL, "", "steps", "total", "move", "paint", "copy",
                  "areas");
    timed_row(out, "Typing a key", &t->typing);
    timed_row(out, "Scroll 32 px, new", &t->small);
    timed_row(out, "Scroll 32 px, again", &t->again);
    timed_row(out, "Page down", &t->page);
    timed_row(out, "Whole page repaint", &t->full);
    if (!t->small.steps)
        au_buf_puts(out, "  The page does not scroll: only the whole page repaint was timed.\n");
    if (t->typing.steps)
        au_report_kv(out, "Typing, 16 keys", "%s restyles (%s elements), %s layouts", s_count(a, sizeof a, t->key_restyles),
                     s_count(b, sizeof b, t->key_elements), s_count(c, sizeof c, t->key_layouts));
    else
        au_buf_puts(out, "  Typing was not timed: click a text field of the page first.\n");
    au_report_kv(out, "Glyph cache", "%s hits, %s misses, %s evictions in these steps", s_count(a, sizeof a, t->glyph_hits),
                 s_count(b, sizeof b, t->glyph_misses), s_count(c, sizeof c, t->glyph_evictions));
}

static void bench_row(AuBuf *out, const char *name, uint32_t tenths, const char *note)
{
    char v[24];
    snprintf(v, sizeof v, "%lu.%lu ms", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    au_buf_printf(out, "  %-*s %10s%s%s\n", AU_REPORT_LABEL + 4, name, v, note[0] ? "   " : "", note);
}

void au_dev_bench_report(const AuPaintBench *b, AuBuf *out)
{
    if (!b || !b->ran)
        return;
    char area[48];
    snprintf(area, sizeof area, "%d x %d pixels", b->width, b->height);
    au_report_heading(out, "Paint benchmark (time per run)");
    bench_row(out, "Test scene, cached glyphs", b->scene_warm, area);
    bench_row(out, "Test scene, no cached glyphs", b->scene_cold, "");
    bench_row(out, "Half-transparent fill", b->fill, area);
    bench_row(out, "Toolbar and tabs", b->chrome, "");
}

/* ---- Performance ---- */

static const char *const kind_titles[AU_PERF_KINDS] = {"Script", "Style", "Layout", "Paint", "Screen copy"};

#define TIME_ROW "  %-18s %7s %7s   %10s %9s   "

static void mem_row(AuBuf *out, const char *name, uint32_t used_kb, uint32_t limit_kb, const char *note)
{
    char u[24], l[24];
    au_buf_printf(out, "  %-18s %10s %10s   ", name, s_kb(u, sizeof u, used_kb), limit_kb ? s_kb(l, sizeof l, limit_kb) : "");
    if (limit_kb) {
        au_put_bar(out, used_kb, limit_kb, 20);
        au_buf_printf(out, " %3u %%", percent(used_kb, limit_kb));
    }
    if (note && note[0])
        au_buf_printf(out, "%s%s", limit_kb ? "  " : "", note);
    /* No spaces at the end of a line. */
    while (out->len && out->data[out->len - 1] == ' ')
        out->len--;
    au_buf_putc(out, '\n');
}

void au_dev_perf_report(AuPage *p, const AuDevHostInfo *h, uint32_t now_ms, AuBuf *out)
{
    AuPerf *pf = au_page_perf(p);
    au_perf_sample(pf, now_ms);
    AuPageStats st;
    au_page_stats(p, &st);
    const AuNetLog *nl = au_page_netlog(p);
    char a[32], b[32], c[32], d[32];

    au_buf_puts(out, "Performance\n");
    au_report_kv(out, "Page", "%s", au_page_url(p));
    if (h && h->machine && h->machine[0])
        au_report_kv(out, "Machine", "%s", h->machine);
    if (h && h->build && h->build[0])
        au_report_kv(out, "Aura", "%s", h->build);

    /* The numbers a person wants first. */
    au_report_heading(out, "Summary");
    {
        AuBuf v;
        au_buf_init(&v);
        if (au_page_loading(p))
            au_buf_puts(&v, "in progress");
        else
            au_put_ms(&v, st.load_ms);
        if (st.first_paint_ms) {
            au_buf_puts(&v, "   (first paint ");
            au_put_ms(&v, st.first_paint_ms);
            if (nl->dcl_ms) {
                au_buf_puts(&v, ", DOMContentLoaded ");
                au_put_ms(&v, nl->dcl_ms);
            }
            au_buf_putc(&v, ')');
        }
        if (st.restored)
            au_buf_printf(&v,
                          "   shown again from the kept documents; hiding the last page took %lu ms, building the "
                          "layout again %lu ms",
                          (unsigned long)st.hide_ms, (unsigned long)st.rebuild_ms);
        au_report_kv(out, "Page load", "%s", au_buf_cstr(&v));
        au_buf_free(&v);
    }
    if (st.source_bytes)
        au_report_kv(out, "Page source", "%lu KB, held in %lu KB (compressed in %lu ms)",
                     (unsigned long)(st.source_bytes / 1024), (unsigned long)(st.source_held / 1024),
                     (unsigned long)st.source_pack_ms);
    au_report_kv(out, "Requests", "%s   (%s decoded, %s blocked, %s failed)", s_count(a, sizeof a, nl->requests),
                 s_size(b, sizeof b, nl->decoded_bytes), s_count(c, sizeof c, nl->blocked),
                 s_count(d, sizeof d, nl->failed));
    uint32_t busy = 0, total = 0;
    for (int k = 0; k < AU_PERF_KINDS; k++) {
        busy += pf->last_ms[k];
        total += pf->ms[k];
    }
    if (pf->interval_ms) {
        char label[64];
        snprintf(label, sizeof label, "Busy, last %s", s_ms(a, sizeof a, pf->interval_ms));
        au_report_kv(out, label, "%u %%   (%s ms of work)", percent(busy, pf->interval_ms), s_count(b, sizeof b, busy));
    }
    au_report_kv(out, "Layout boxes", "%s", s_count(a, sizeof a, st.boxes));
    if (h && h->have_memory) {
        if (h->partition_kb)
            au_report_kv(out, "Free memory", "%s of %s   (largest block %s)", s_kb(a, sizeof a, h->heap_free_kb),
                         s_kb(b, sizeof b, h->partition_kb), s_kb(c, sizeof c, h->heap_block_kb));
        else
            au_report_kv(out, "Free memory", "%s   (largest block %s)", s_kb(a, sizeof a, h->heap_free_kb),
                         s_kb(c, sizeof c, h->heap_block_kb));
    }

    /* Where the time went. */
    {
        char title[96];
        if (pf->interval_ms)
            snprintf(title, sizeof title, "Time                  last %-13s since the page began", s_ms(a, sizeof a, pf->interval_ms));
        else
            snprintf(title, sizeof title, "Time                  last interval      since the page began");
        au_report_heading(out, title);
    }
    au_buf_printf(out, TIME_ROW "%s\n", "", "ms", "runs", "ms", "runs", "share of the time");
    for (int k = 0; k < AU_PERF_KINDS; k++) {
        au_buf_printf(out, TIME_ROW, kind_titles[k], s_count(a, sizeof a, pf->last_ms[k]),
                      s_count(b, sizeof b, pf->last_count[k]), s_count(c, sizeof c, pf->ms[k]),
                      s_count(d, sizeof d, pf->count[k]));
        au_put_bar(out, pf->ms[k], total, 20);
        au_buf_printf(out, " %3u %%\n", percent(pf->ms[k], total));
    }
    au_buf_printf(out, TIME_ROW, "All", s_count(a, sizeof a, busy), "", s_count(c, sizeof c, total), "");
    while (out->len && out->data[out->len - 1] == ' ')
        out->len--; /* the empty cells */
    au_buf_putc(out, '\n');

    au_report_heading(out, "Work done");
    if (pf->kpixels >= 1000)
        au_report_kv(out, "Pixels painted", "%lu.%lu million", (unsigned long)(pf->kpixels / 1000),
                     (unsigned long)(pf->kpixels % 1000 / 100));
    else
        au_report_kv(out, "Pixels painted", "%lu thousand", (unsigned long)pf->kpixels);
    AuDocViewStats ds;
    if (au_page_update_stats(p, &ds)) {
        au_report_kv(out, "Style passes", "%s   (%s elements styled)", s_count(a, sizeof a, ds.style_passes),
                     s_count(b, sizeof b, ds.styled_elements));
        au_report_kv(out, "Box tree builds", "%s", s_count(a, sizeof a, ds.tree_builds));
        au_report_kv(out, "Subtree rebuilds", "%s   (%s boxes)", s_count(a, sizeof a, ds.subtree_builds),
                     s_count(b, sizeof b, ds.subtree_boxes));
        au_report_kv(out, "Layouts", "%s", s_count(a, sizeof a, ds.layouts));
    }
    au_report_kv(out, "Image decodes", "%s   (%s dropped to make room)", s_count(a, sizeof a, st.image_decodes),
                 s_count(b, sizeof b, st.image_evictions));

    /* Memory, each line against its limit. */
    au_report_heading(out, "Memory                   used      limit");
    if (h && h->have_memory && h->partition_kb)
        mem_row(out, "Application heap",
                h->partition_kb > h->heap_free_kb ? h->partition_kb - h->heap_free_kb : 0, h->partition_kb, NULL);
    size_t js_used, js_limit;
    au_page_script_memory(p, &js_used, &js_limit);
    if (js_limit)
        mem_row(out, "Script heap", (uint32_t)((js_used + 1023) / 1024), (uint32_t)(js_limit / 1024), NULL);
    else
        au_buf_printf(out, "  %-18s %10s\n", "Script heap", "none");
    mem_row(out, "Decoded images", st.image_kb, st.image_budget_kb, NULL);
    if (h && h->have_net) {
        char note[48];
        snprintf(note, sizeof note, "%s entries", s_count(a, sizeof a, h->http_cache_entries));
        mem_row(out, "HTTP memory cache", h->http_cache_kb, h->http_cache_max_kb, note);
    }
    AuGlyphCacheStats gs;
    au_glyph_cache_stats(&gs);
    {
        char note[120];
        snprintf(note, sizeof note, "%s glyphs", s_count(a, sizeof a, (unsigned long)gs.entries));
        mem_row(out, "Glyph cache", (uint32_t)(gs.bytes / 1024), 0, note);
        snprintf(note, sizeof note, "%s kept for Back and Forward", s_count(a, sizeof a, (unsigned long)au_page_kept_count(p)));
        mem_row(out, "Kept documents", (uint32_t)(au_page_kept_bytes(p) / 1024), 0, note);
    }
    if (h && h->have_memory) {
        mem_row(out, "Temporary memory", h->temp_free_kb, 0, "free");
        /* What the allocator took from temporary memory because the heap
         * was short. */
        if (h->temp_used_kb)
            mem_row(out, "  taken by Aura", h->temp_used_kb, 0, "of temporary memory, for the heap");
        /* The blocks that outlive pages (network, cache, fonts, atoms),
         * kept out of the heap's free space. */
        if (h->long_kb) {
            char note[64];
            snprintf(note, sizeof note, "%lu KB of it in temporary memory", (unsigned long)h->long_temp_kb);
            mem_row(out, "Long-lived blocks", h->long_kb, 0, note);
        }
    }
    /* Each script runtime, document and worker of the page. */
    {
        AuBuf rt;
        au_buf_init(&rt);
        au_page_script_summary(p, &rt);
        if (rt.len) {
            au_report_heading(out, "Script runtimes");
            au_buf_append(out, rt.data, rt.len);
            if (rt.data[rt.len - 1] != '\n')
                au_buf_putc(out, '\n');
        }
        au_buf_free(&rt);
    }

    if (h && h->have_net) {
        au_report_heading(out, "Network");
        au_report_kv(out, "HTTP cache lookups", "%lu fresh, %lu history, %lu revalidated, %lu not modified, %lu missed",
                     h->cache_fresh, h->cache_history, h->cache_stale, h->cache_304, h->cache_missed);
        au_report_kv(out, "HTTP cache writes", "%lu stored, %lu read from disk", h->cache_stored, h->cache_disk);
        au_report_kv(out, "DNS", "%lu lookups, %lu from the cache, %lu shared, %lu requests, %lu answers held",
                     h->dns_lookups, h->dns_hits, h->dns_shared, h->dns_requests, h->dns_entries);
        if (h->connection_text && h->connection_text[0])
            au_report_kv(out, "Connections", "%d open, %d busy: %s", h->connections, h->connections_busy,
                         h->connection_text);
        else
            au_report_kv(out, "Connections", "%d open, %d busy", h->connections, h->connections_busy);
    }

    AuPerfTask slow[8];
    int n = au_perf_slowest(pf, slow, 8);
    {
        char title[64];
        snprintf(title, sizeof title, "Slowest recent tasks (%d ms or more)", AU_PERF_SLOW_MS);
        au_report_heading(out, title);
    }
    if (!n)
        au_buf_puts(out, "  none\n");
    for (int i = 0; i < n; i++) {
        au_buf_printf(out, "  %-12s %9s", kind_titles[slow[i].kind < AU_PERF_KINDS ? slow[i].kind : 0],
                      s_ms(a, sizeof a, slow[i].ms));
        if (slow[i].label[0])
            au_buf_printf(out, "   %s", slow[i].label);
        au_buf_putc(out, '\n');
    }

    if (h)
        au_dev_timing_report(h->timing, out);

    au_report_heading(out, "What the numbers mean");
    au_buf_puts(out, "  Time      Milliseconds of work and how many times it ran, in the last\n"
                     "            interval and since the page began. Script includes the style and\n"
                     "            layout that script forces; those also count on their own lines.\n"
                     "  Runs      Script: entries into script. Style, Layout, Paint: passes.\n"
                     "            Screen copy: copies of painted pixels to the window.\n"
                     "  Memory    What each store holds now, against its limit where it has one.\n"
                     "  Tasks     Single runs that kept the browser from answering for 50 ms or more.\n");
}

/* ---- Console ---- */

const char *au_dev_console_label(int level)
{
    static const char *const names[] = {"log", "info", "warn", "error", "debug", ">", "<"};
    return level >= 0 && level < 7 ? names[level] : "log";
}

/* Finds "URL:LINE" or "URL:LINE:COL" at the end of a stack frame ("at f
 * (URL:12:5)", "at URL:12:5") in the text after its first line. */
static void find_source(const char *text, AuConsoleEntry *e)
{
    const char *s = strstr(text, "\n    at ");
    if (!s)
        return;
    s += 8;
    size_t n = strcspn(s, "\n");
    const char *open = memchr(s, '(', n);
    if (open) {
        n -= (size_t)(open + 1 - s);
        s = open + 1;
        if (n && s[n - 1] == ')')
            n--;
    }
    /* Strip ":COL" then ":LINE" from the end. */
    int nums[2] = {0, 0}, got = 0;
    while (got < 2 && n) {
        size_t k = n;
        while (k && s[k - 1] >= '0' && s[k - 1] <= '9')
            k--;
        if (k == n || !k || s[k - 1] != ':')
            break;
        int v = 0;
        for (size_t i = k; i < n; i++)
            v = v < 100000000 ? v * 10 + (s[i] - '0') : v;
        nums[got++] = v;
        n = k - 1;
    }
    if (!got || !n)
        return;
    e->line = got == 2 ? nums[1] : nums[0];
    snprintf(e->source, sizeof e->source, "%.*s", (int)(n < sizeof e->source - 1 ? n : sizeof e->source - 1), s);
}

bool au_dev_console_entry(const AuPage *p, size_t i, AuConsoleEntry *out)
{
    memset(out, 0, sizeof *out);
    out->text = au_page_console_line(p, i, &out->level);
    if (!out->text)
        return false;
    if (out->level <= 4)
        find_source(out->text, out);
    return true;
}

bool au_dev_console_shows(const AuConsoleFilter *f, const AuConsoleEntry *e)
{
    if (!f)
        return true;
    if (e->level != AU_CONSOLE_INPUT && e->level != AU_CONSOLE_RESULT) {
        unsigned bit = e->level == 3 ? AU_CONSOLE_SHOW_ERRORS : e->level == 2 ? AU_CONSOLE_SHOW_WARNINGS : AU_CONSOLE_SHOW_LOGS;
        if (!(f->show & bit))
            return false;
    }
    return !f->text || !f->text[0] || au_text_contains_ci(e->text, f->text);
}

/* An entry's lines: the first with its label and source, the stack
 * indented. add is called for each. */
typedef void (*LineFn)(void *ctx, int level, const char *text, size_t len);

static void entry_lines(const AuConsoleEntry *e, AuBuf *b, LineFn add, void *ctx)
{
    const char *s = e->text;
    size_t first = strcspn(s, "\n");
    au_buf_clear(b);
    au_buf_printf(b, "%-5s  ", au_dev_console_label(e->level));
    au_buf_append(b, s, first);
    if (e->source[0])
        au_buf_printf(b, "  (%s:%d)", e->source, e->line);
    add(ctx, e->level, au_buf_cstr(b), b->len);
    s += first;
    while (*s == '\n') {
        s++;
        size_t k = strcspn(s, "\n");
        au_buf_clear(b);
        au_buf_puts(b, "       ");
        au_buf_append(b, s, k);
        add(ctx, e->level, au_buf_cstr(b), b->len);
        s += k;
    }
}

static void add_to_buf(void *ctx, int level, const char *text, size_t len)
{
    (void)level;
    au_buf_append((AuBuf *)ctx, text, len);
    au_buf_putc((AuBuf *)ctx, '\n');
}

void au_dev_console_text(const AuPage *p, AuBuf *out)
{
    AuBuf b;
    au_buf_init(&b);
    size_t n = au_page_console_count(p);
    for (size_t i = 0; i < n; i++) {
        AuConsoleEntry e;
        if (au_dev_console_entry(p, i, &e))
            entry_lines(&e, &b, add_to_buf, out);
    }
    au_buf_free(&b);
}

/* ---- Lines for the text view ---- */

static int console_style(int level)
{
    switch (level) {
    case 2: return AU_TS_WARN;
    case 3: return AU_TS_ERROR;
    case 4: return AU_TS_DIM;
    case AU_CONSOLE_INPUT: return AU_TS_INPUT;
    case AU_CONSOLE_RESULT: return AU_TS_RESULT;
    default: return AU_TS_PLAIN;
    }
}

static void add_to_list(void *ctx, int level, const char *text, size_t len)
{
    au_textlist_add((AuTextList *)ctx, console_style(level), text, len);
}

uint32_t au_dev_console_lines(const AuPage *p, const AuConsoleFilter *f, AuTextList *out)
{
    au_textlist_clear(out);
    AuBuf b;
    au_buf_init(&b);
    uint32_t hidden = 0;
    size_t n = au_page_console_count(p), cur = au_page_console_current(p);
    for (size_t i = 0; i < n; i++) {
        /* Where the lines of the page now shown begin, when older lines
         * are above them. */
        if (i == n - cur && i > 0) {
            au_buf_clear(&b);
            au_buf_printf(&b, "Page: %s", au_page_url(p));
            au_textlist_add(out, AU_TS_HEADING, au_buf_cstr(&b), b.len);
        }
        AuConsoleEntry e;
        if (!au_dev_console_entry(p, i, &e))
            continue;
        if (!au_dev_console_shows(f, &e)) {
            hidden++;
            continue;
        }
        entry_lines(&e, &b, add_to_list, out);
    }
    au_buf_free(&b);
    return hidden;
}

void au_dev_network_lines(const AuNetLog *l, const AuNetView *v, int cells, AuTextList *out)
{
    au_textlist_clear(out);
    AuBuf b;
    au_buf_init(&b);
    for (uint32_t i = 0; i < v->n; i++) {
        const AuNetRecord *r = au_netlog_at(l, v->rows[i]);
        if (!r)
            continue;
        au_buf_clear(&b);
        au_netlog_row_fit(r, cells, &b);
        int style = au_netrec_problem(r)                                                  ? AU_TS_ERROR
                    : r->state == AU_NETREC_PENDING || r->state == AU_NETREC_CANCELLED ? AU_TS_DIM
                                                                                       : AU_TS_PLAIN;
        au_textlist_add(out, style, au_buf_cstr(&b), b.len);
    }
    au_buf_free(&b);
}

void au_dev_network_detail_lines(const AuNetRecord *r, AuTextList *out)
{
    au_textlist_clear(out);
    if (!r)
        return;
    AuBuf b;
    au_buf_init(&b);
    au_netlog_detail(r, &b);
    au_dev_report_lines(au_buf_cstr(&b), out);
    au_buf_free(&b);
}

void au_dev_network_summary_lines(const AuNetLog *l, const char *connections, AuTextList *out)
{
    au_textlist_clear(out);
    if (l) {
        AuBuf b;
        au_buf_init(&b);
        au_netlog_summary(l, connections, &b);
        au_dev_report_lines(au_buf_cstr(&b), out);
        au_buf_free(&b);
    }
    au_textlist_add_text(out, AU_TS_DIM, "Click a request to see its details and headers. Click a column title to sort.");
}

void au_dev_report_lines(const char *text, AuTextList *out)
{
    au_textlist_clear(out);
    const char *s = text;
    while (*s) {
        size_t n = strcspn(s, "\n");
        const char *next = s[n] ? s + n + 1 : s + n;
        bool heading = n > 0 && s[0] != ' ' && next[0] == ' ' && next[1] == ' ';
        au_textlist_add(out, heading ? AU_TS_HEADING : AU_TS_PLAIN, s, n);
        s = next;
    }
}

void au_dev_markdown_lines(const char *text, AuTextList *out)
{
    au_textlist_clear(out);
    const char *s = text;
    while (*s) {
        size_t n = strcspn(s, "\n");
        int style = AU_TS_PLAIN;
        if (s[0] == '#')
            style = AU_TS_HEADING;
        else if (!strncmp(s, "```", 3) || !strncmp(s, "<details>", 9) || !strncmp(s, "</details>", 10) ||
                 !strncmp(s, "<summary>", 9) || !strncmp(s, "<!--", 4))
            style = AU_TS_DIM;
        au_textlist_add(out, style, s, n);
        s += n + (s[n] == '\n');
    }
}

bool au_dev_console_eval(AuPage *p, const char *expr)
{
    AuBuf r;
    au_buf_init(&r);
    au_page_console_add(p, AU_CONSOLE_INPUT, expr);
    bool ok = au_page_eval(p, expr, &r);
    au_page_console_add(p, ok ? AU_CONSOLE_RESULT : 3, au_buf_cstr(&r));
    au_buf_free(&r);
    return ok;
}
