/* The developer tools' text (see devtools.h). */
#include "devtools.h"
#include "netlog.h"
#include "textview.h"

#include "../image/cache.h"
#include "../page/page.h" /* AuDocViewStats */
#include "../page/perf.h"

#include <stdio.h>
#include <string.h>

/* ---- Performance ---- */

static void put_row(AuBuf *out, const char *name, uint32_t last_ms, uint32_t last_n, uint32_t ms, uint32_t n)
{
    char a[32], b[32];
    snprintf(a, sizeof a, "%lu ms", (unsigned long)last_ms);
    snprintf(b, sizeof b, "%lu ms", (unsigned long)ms);
    au_buf_printf(out, "  %-12s %10s %7lu %12s %8lu\n", name, a, (unsigned long)last_n, b, (unsigned long)n);
}

static void put_kb(AuBuf *out, size_t bytes)
{
    au_buf_printf(out, "%lu KB", (unsigned long)((bytes + 1023) / 1024));
}

void au_dev_perf_report(AuPage *p, const AuDevHostInfo *h, uint32_t now_ms, AuBuf *out)
{
    AuPerf *pf = au_page_perf(p);
    au_perf_sample(pf, now_ms);
    AuPageStats st;
    au_page_stats(p, &st);
    const AuNetLog *nl = au_page_netlog(p);

    au_buf_printf(out, "Aura performance report\nPage: %s\n", au_page_url(p));
    if (h && h->machine && h->machine[0])
        au_buf_printf(out, "Machine: %s\n", h->machine);
    au_buf_puts(out, "Load: ");
    if (au_page_loading(p))
        au_buf_puts(out, "in progress");
    else {
        au_buf_puts(out, "done in ");
        au_put_ms(out, st.load_ms);
    }
    if (st.first_paint_ms) {
        au_buf_puts(out, ", first paint ");
        au_put_ms(out, st.first_paint_ms);
    }
    if (nl->dcl_ms) {
        au_buf_puts(out, ", DOMContentLoaded ");
        au_put_ms(out, nl->dcl_ms);
    }
    au_buf_printf(out, "; %lu requests, ", (unsigned long)nl->requests);
    au_put_size(out, nl->decoded_bytes);
    au_buf_printf(out, "; %lu boxes\n\n", (unsigned long)st.boxes);

    au_buf_puts(out, "Time");
    if (pf->interval_ms) {
        au_buf_puts(out, " (last ");
        au_put_ms(out, pf->interval_ms);
        au_buf_puts(out, ", and since the document began)");
    }
    au_buf_putc(out, '\n');
    au_buf_printf(out, "  %-12s %10s %7s %12s %8s\n", "", "last", "passes", "total", "passes");
    for (int k = 0; k < AU_PERF_KINDS; k++)
        put_row(out, au_perf_kind_name(k), pf->last_ms[k], pf->last_count[k], pf->ms[k], pf->count[k]);
    au_buf_printf(out, "  pixels painted: %lu thousand", (unsigned long)pf->kpixels);
    AuDocViewStats ds;
    if (au_page_update_stats(p, &ds))
        au_buf_printf(out, "\n  since the view was made: %lu style passes, %lu elements styled, %lu box tree builds, "
                           "%lu layouts",
                      (unsigned long)ds.style_passes, (unsigned long)ds.styled_elements,
                      (unsigned long)ds.tree_builds, (unsigned long)ds.layouts);
    au_buf_puts(out, "\n  (script time includes the style and layout that script forces)\n\n");

    au_buf_puts(out, "Memory\n");
    if (h && h->have_memory)
        au_buf_printf(out, "  application heap: %lu KB free (largest block %lu KB); temporary memory: %lu KB free, %lu KB "
                           "taken by Aura's heap\n",
                      (unsigned long)h->heap_free_kb, (unsigned long)h->heap_block_kb,
                      (unsigned long)h->temp_free_kb, (unsigned long)h->temp_used_kb);
    size_t js_used, js_limit;
    au_page_script_memory(p, &js_used, &js_limit);
    au_buf_puts(out, "  script heap: ");
    if (js_limit) {
        put_kb(out, js_used);
        au_buf_puts(out, " of ");
        put_kb(out, js_limit);
    } else {
        au_buf_puts(out, "no runtime");
    }
    au_buf_putc(out, '\n');
    au_page_script_summary(p, out);
    au_buf_printf(out, "  images: %lu KB decoded of %lu KB (%lu decodes, %lu evictions)\n",
                  (unsigned long)st.image_kb, (unsigned long)st.image_budget_kb, (unsigned long)st.image_decodes,
                  (unsigned long)st.image_evictions);
    au_buf_printf(out, "  documents kept for Back and Forward: %lu, about %lu KB",
                  (unsigned long)au_page_kept_count(p), (unsigned long)(au_page_kept_bytes(p) / 1024));
    if (st.restored)
        au_buf_printf(out, " (this one was restored; hiding the last took %lu ms, building its layout again %lu ms)",
                      (unsigned long)st.hide_ms, (unsigned long)st.rebuild_ms);
    au_buf_putc(out, '\n');
    AuGlyphCacheStats gs;
    au_glyph_cache_stats(&gs);
    au_buf_printf(out, "  glyph cache: %lu KB, %lu glyphs (%lu hits, %lu misses, %lu evictions)\n",
                  (unsigned long)(gs.bytes / 1024), (unsigned long)gs.entries, (unsigned long)gs.hits,
                  (unsigned long)gs.misses, (unsigned long)gs.evictions);
    if (h && h->have_net) {
        au_buf_printf(out,
                      "  HTTP cache: %lu KB of %lu KB, %lu entries (%lu fresh, %lu history, %lu revalidated, "
                      "%lu not modified, %lu missed, %lu stored, %lu read from disk)\n",
                      (unsigned long)h->http_cache_kb, (unsigned long)h->http_cache_max_kb,
                      (unsigned long)h->http_cache_entries, h->cache_fresh, h->cache_history, h->cache_stale,
                      h->cache_304, h->cache_missed, h->cache_stored, h->cache_disk);
        au_buf_printf(out, "  DNS: %lu lookups, %lu from the cache, %lu shared, %lu requests, %lu answers held\n",
                      h->dns_lookups, h->dns_hits, h->dns_shared, h->dns_requests, h->dns_entries);
        au_buf_printf(out, "\nConnections: %d open, %d busy", h->connections, h->connections_busy);
        if (h->connection_text && h->connection_text[0])
            au_buf_printf(out, ": %s", h->connection_text);
        au_buf_putc(out, '\n');
    }

    AuPerfTask slow[8];
    int n = au_perf_slowest(pf, slow, 8);
    au_buf_printf(out, "\nSlowest recent tasks (%d ms or more)\n", AU_PERF_SLOW_MS);
    if (!n)
        au_buf_puts(out, "  none\n");
    for (int i = 0; i < n; i++) {
        au_buf_printf(out, "  %s", au_perf_kind_name(slow[i].kind));
        if (slow[i].label[0])
            au_buf_printf(out, " %s", slow[i].label);
        au_buf_puts(out, " ");
        au_put_ms(out, slow[i].ms);
        au_buf_putc(out, '\n');
    }
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

void au_dev_console_text(const AuPage *p, AuBuf *out)
{
    size_t n = au_page_console_count(p);
    for (size_t i = 0; i < n; i++) {
        AuConsoleEntry e;
        if (!au_dev_console_entry(p, i, &e))
            continue;
        const char *s = e.text;
        size_t first = strcspn(s, "\n");
        au_buf_printf(out, "%-5s ", au_dev_console_label(e.level));
        au_buf_append(out, s, first);
        if (e.source[0])
            au_buf_printf(out, "  (%s:%d)", e.source, e.line);
        au_buf_putc(out, '\n');
        s += first;
        while (*s == '\n') {
            s++;
            size_t k = strcspn(s, "\n");
            au_buf_puts(out, "      ");
            au_buf_append(out, s, k);
            au_buf_putc(out, '\n');
            s += k;
        }
    }
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

void au_dev_console_lines(const AuPage *p, AuTextList *out)
{
    au_textlist_clear(out);
    AuBuf b;
    au_buf_init(&b);
    size_t n = au_page_console_count(p);
    for (size_t i = 0; i < n; i++) {
        AuConsoleEntry e;
        if (!au_dev_console_entry(p, i, &e))
            continue;
        int style = console_style(e.level);
        const char *s = e.text;
        size_t first = strcspn(s, "\n");
        au_buf_clear(&b);
        au_buf_printf(&b, "%-5s ", au_dev_console_label(e.level));
        au_buf_append(&b, s, first);
        if (e.source[0])
            au_buf_printf(&b, "  (%s:%d)", e.source, e.line);
        au_textlist_add(out, style, au_buf_cstr(&b), b.len);
        s += first;
        while (*s == '\n') {
            s++;
            size_t k = strcspn(s, "\n");
            au_buf_clear(&b);
            au_buf_puts(&b, "      ");
            au_buf_append(&b, s, k);
            au_textlist_add(out, style, au_buf_cstr(&b), b.len);
            s += k;
        }
    }
    au_buf_free(&b);
}

void au_dev_network_lines(const AuNetLog *l, AuTextList *out)
{
    au_textlist_clear(out);
    AuBuf b;
    au_buf_init(&b);
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        au_buf_clear(&b);
        au_netlog_row(r, &b);
        int style = r->state == AU_NETREC_FAILED || r->state == AU_NETREC_BLOCKED || r->status >= 400 ? AU_TS_ERROR
                    : r->state == AU_NETREC_PENDING || r->state == AU_NETREC_CANCELLED                 ? AU_TS_DIM
                                                                                                       : AU_TS_PLAIN;
        au_textlist_add(out, style, au_buf_cstr(&b), b.len);
    }
    au_buf_free(&b);
}

void au_dev_network_detail_lines(const AuNetRecord *r, AuTextList *out)
{
    au_textlist_clear(out);
    if (!r) {
        au_textlist_add_text(out, AU_TS_DIM, "Select a request to see its details and headers.");
        return;
    }
    AuBuf b;
    au_buf_init(&b);
    au_netlog_detail(r, &b);
    au_dev_report_lines(au_buf_cstr(&b), out);
    au_buf_free(&b);
}

void au_dev_network_blocked_lines(const AuNetLog *l, AuTextList *out)
{
    au_textlist_clear(out);
    if (!l || !l->blocked) {
        au_textlist_add_text(out, AU_TS_DIM, "Select a request to see its details and headers.");
        return;
    }
    AuBuf b;
    au_buf_init(&b);
    au_netlog_blocked(l, &b);
    const char *s = au_buf_cstr(&b);
    bool first = true;
    while (*s) {
        size_t n = strcspn(s, "\n");
        au_textlist_add(out, first ? AU_TS_HEADING : AU_TS_PLAIN, s, n);
        first = false;
        s += n + (s[n] == '\n');
    }
    au_buf_free(&b);
    au_textlist_add_text(out, AU_TS_DIM, "Select a request to see its details and headers.");
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
