/* The request log of a page (see netlog.h). */
#include "netlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void au_netlog_init(AuNetLog *l)
{
    memset(l, 0, sizeof *l);
    l->first_seq = 1;
}

static void rec_free(AuNetRecord *r)
{
    au_free(r->url);
    au_free(r->request_head);
    au_free(r->response_head);
    memset(r, 0, sizeof *r);
}

void au_netlog_clear(AuNetLog *l)
{
    for (uint32_t i = 0; i < l->cap; i++)
        if (l->recs[i].url)
            rec_free(&l->recs[i]);
    uint32_t gen = l->generation + 1, cap = l->cap;
    AuNetRecord *recs = l->recs;
    memset(l, 0, sizeof *l);
    l->recs = recs;
    l->cap = cap;
    l->first_seq = 1;
    l->generation = gen;
}

void au_netlog_free(AuNetLog *l)
{
    au_netlog_clear(l);
    au_free(l->recs);
    l->recs = NULL;
    l->cap = 0;
}

static AuNetRecord *find(AuNetLog *l, uint32_t seq)
{
    if (!l->recs || seq < l->first_seq || seq - l->first_seq >= l->count)
        return NULL;
    return &l->recs[(seq - 1) % AU_NETLOG_MAX];
}

const AuNetRecord *au_netlog_at(const AuNetLog *l, uint32_t i)
{
    if (!l->recs || i >= l->count)
        return NULL;
    return &l->recs[(l->first_seq + i - 1) % AU_NETLOG_MAX];
}

static void copy_text(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

uint32_t au_netlog_begin(AuNetLog *l, const char *url, const char *method, AuFetchKind kind, uint32_t start_ms)
{
    uint32_t seq = ++l->requests;
    uint32_t slot = (seq - 1) % AU_NETLOG_MAX;
    if (slot >= l->cap) {
        /* The first AU_NETLOG_MAX records fill the array in order, so it
         * grows as they come: a page with thirty requests keeps room for
         * thirty-two. */
        uint32_t cap = l->cap ? l->cap * 2 : 16;
        if (cap > AU_NETLOG_MAX)
            cap = AU_NETLOG_MAX;
        l->recs = au_realloc(l->recs, cap * sizeof *l->recs);
        memset(l->recs + l->cap, 0, (cap - l->cap) * sizeof *l->recs);
        l->cap = cap;
    }
    AuNetRecord *r = &l->recs[slot];
    if (l->count == AU_NETLOG_MAX) {
        rec_free(r); /* the oldest record's slot */
        l->first_seq++;
    } else {
        l->count++;
    }
    memset(r, 0, sizeof *r);
    r->seq = seq;
    size_t n = strlen(url);
    r->url = au_strndup(url, n < 2048 ? n : 2048);
    copy_text(r->method, sizeof r->method, method && *method ? method : "GET");
    r->kind = (uint8_t)kind;
    r->state = AU_NETREC_PENDING;
    r->source = AU_NETREC_FROM_UNKNOWN;
    r->start_ms = start_ms;
    r->first_ms = r->total_ms = -1;
    for (int i = 0; i <= AU_FETCH_PHASE_DONE; i++)
        r->at[i] = -1;
    l->generation++;
    return seq;
}

void au_netlog_set_url(AuNetLog *l, uint32_t seq, const char *url)
{
    AuNetRecord *r = find(l, seq);
    if (!r || !url || !*url || !strcmp(r->url, url))
        return;
    size_t n = strlen(url);
    au_free(r->url);
    r->url = au_strndup(url, n < 2048 ? n : 2048);
}

void au_netlog_source(AuNetLog *l, uint32_t seq, int source)
{
    AuNetRecord *r = find(l, seq);
    if (r)
        r->source = (uint8_t)source;
}

void au_netlog_response(AuNetLog *l, uint32_t seq, int status, const char *type, bool from_cache, uint32_t now_ms)
{
    AuNetRecord *r = find(l, seq);
    if (!r)
        return;
    r->status = status;
    copy_text(r->type, sizeof r->type, type);
    if (r->source == AU_NETREC_FROM_UNKNOWN)
        r->source = from_cache ? AU_FETCH_FROM_MEMORY : AU_FETCH_FROM_NETWORK;
    r->first_ms = (int32_t)(now_ms - r->start_ms);
    l->generation++;
}

void au_netlog_data(AuNetLog *l, uint32_t seq, size_t len)
{
    l->decoded_bytes += (uint32_t)len;
    AuNetRecord *r = find(l, seq);
    if (r)
        r->decoded += (uint32_t)len;
    /* No generation bump: data arrives often, and the end bumps it. */
}

static char *dup_head(const char *s)
{
    if (!s || !*s)
        return NULL;
    size_t n = strlen(s);
    return au_strndup(s, n < AU_NETLOG_HEAD_MAX ? n : AU_NETLOG_HEAD_MAX);
}

void au_netlog_end(AuNetLog *l, uint32_t seq, AuNetRecState state, const AuFetchInfo *info, uint32_t now_ms)
{
    const AuFetchNet *n = info ? info->net : NULL;
    if (n && n->blocked_reason[0] && state == AU_NETREC_FAILED)
        state = AU_NETREC_BLOCKED;
    if (info && info->error == AU_FETCH_ERR_BLOCKED && state == AU_NETREC_FAILED)
        state = AU_NETREC_BLOCKED;
    l->finished++;
    if (state == AU_NETREC_FAILED)
        l->failed++;
    else if (state == AU_NETREC_BLOCKED)
        l->blocked++;
    if (n) {
        l->transfer_bytes += (uint32_t)n->transfer_bytes;
        if (n->source != AU_FETCH_FROM_NETWORK && state == AU_NETREC_DONE)
            l->from_cache++;
    }
    l->generation++;
    AuNetRecord *r = find(l, seq);
    if (!r)
        return;
    r->state = (uint8_t)state;
    r->total_ms = (int32_t)(now_ms - r->start_ms);
    if (info) {
        /* After redirects and the HTTPS-first upgrade the record names
         * the address that answered. */
        if (info->error != AU_FETCH_ERR_HTTPS_FALLBACK)
            au_netlog_set_url(l, seq, info->url);
        r->error = (uint8_t)info->error;
        if (info->status)
            r->status = info->status;
        if (info->content_type && !r->type[0])
            copy_text(r->type, sizeof r->type, info->content_type);
        if (info->mixed && !r->blocked_reason[0])
            copy_text(r->blocked_reason, sizeof r->blocked_reason, "mixed content");
    }
    if (!n)
        return;
    r->have_net = true;
    for (int i = 0; i <= AU_FETCH_PHASE_DONE; i++)
        r->at[i] = n->at[i];
    if (r->source == AU_NETREC_FROM_UNKNOWN || r->source <= AU_FETCH_FROM_304)
        r->source = (uint8_t)n->source;
    r->reused = n->reused;
    r->connection = n->connection;
    r->transfer = (uint32_t)n->transfer_bytes;
    r->redirects = n->redirects;
    copy_text(r->remote, sizeof r->remote, n->remote);
    copy_text(r->protocol, sizeof r->protocol, n->protocol);
    copy_text(r->cipher, sizeof r->cipher, n->cipher);
    if (n->blocked_reason[0])
        copy_text(r->blocked_reason, sizeof r->blocked_reason, n->blocked_reason);
    au_free(r->request_head);
    au_free(r->response_head);
    r->request_head = dup_head(n->request_head);
    r->response_head = dup_head(n->response_head);
}

void au_netlog_set_blocked_reason(AuNetLog *l, uint32_t seq, const char *reason)
{
    AuNetRecord *r = find(l, seq);
    if (!r)
        return;
    copy_text(r->blocked_reason, sizeof r->blocked_reason, reason);
    l->generation++;
}

void au_netlog_mark(AuNetLog *l, uint32_t dcl_ms, uint32_t load_ms)
{
    if (dcl_ms)
        l->dcl_ms = dcl_ms;
    if (load_ms)
        l->load_ms = load_ms;
    l->generation++;
}

/* ---- Text ---- */

void au_put_size(AuBuf *out, uint32_t bytes)
{
    if (bytes < 1024)
        au_buf_printf(out, "%lu B", (unsigned long)bytes);
    else if (bytes < 1024u * 1024)
        au_buf_printf(out, "%lu.%lu KB", (unsigned long)(bytes / 1024), (unsigned long)(bytes % 1024 * 10 / 1024));
    else
        au_buf_printf(out, "%lu.%02lu MB", (unsigned long)(bytes / (1024u * 1024)),
                      (unsigned long)(bytes % (1024u * 1024) / 10486));
}

void au_put_ms(AuBuf *out, uint32_t ms)
{
    if (ms < 1000)
        au_buf_printf(out, "%lu ms", (unsigned long)ms);
    else
        au_buf_printf(out, "%lu.%02lu s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
}

void au_put_count(AuBuf *out, unsigned long n)
{
    char d[32];
    int k = 0;
    do {
        d[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && k < 31);
    for (int i = k - 1; i >= 0; i--) {
        au_buf_putc(out, d[i]);
        if (i && i % 3 == 0)
            au_buf_putc(out, ',');
    }
}

void au_report_heading(AuBuf *out, const char *title)
{
    if (out->len)
        au_buf_putc(out, '\n');
    au_buf_puts(out, title);
    au_buf_putc(out, '\n');
}

void au_report_kv(AuBuf *out, const char *label, const char *fmt, ...)
{
    char v[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    au_buf_printf(out, "  %-*s %s\n", AU_REPORT_LABEL, label, v);
}

void au_put_bar(AuBuf *out, uint32_t part, uint32_t whole, int cells)
{
    int fill = 0;
    if (whole) {
        /* In floating point: part * cells overflows 32 bits for byte counts. */
        fill = (int)((double)part * (double)cells / (double)whole + 0.5);
        if (fill > cells)
            fill = cells;
        if (!fill && part)
            fill = 1; /* something, however small, shows */
    }
    au_buf_putc(out, '[');
    for (int i = 0; i < cells; i++)
        au_buf_putc(out, i < fill ? '#' : '.');
    au_buf_putc(out, ']');
}

/* What the request was for. The document view asks for scripts, fonts
 * and script fetches as "other"; the response's type says which. */
static const char *kind_name(const AuNetRecord *r)
{
    static const char *const names[] = {"document", "style", "image", "font", "script", "other", "frame"};
    int k = r->kind;
    if (k == AU_FETCH_OTHER) {
        if (strstr(r->type, "javascript") || strstr(r->type, "ecmascript"))
            return "script";
        if (!strncmp(r->type, "font/", 5) || strstr(r->type, "font-"))
            return "font";
        if (strstr(r->type, "json"))
            return "json";
    }
    return k >= 0 && k < 7 ? names[k] : "other";
}

static const char *source_name(int s)
{
    switch (s) {
    case AU_FETCH_FROM_NETWORK: return "network";
    case AU_FETCH_FROM_MEMORY: return "memory";
    case AU_FETCH_FROM_DISK: return "disk";
    case AU_FETCH_FROM_HISTORY: return "history";
    case AU_FETCH_FROM_304: return "304";
    case AU_NETREC_FROM_FILE: return "file";
    case AU_NETREC_FROM_DATA: return "data";
    default: return "-";
    }
}

static const char *error_name(int e)
{
    static const char *const names[] = {"ok",               "bad URL",          "unsupported scheme",
                                        "host not found",   "connection failed", "timed out",
                                        "connection reset", "protocol error",   "too large",
                                        "too many redirects", "content decoding failed", "TLS failure",
                                        "certificate error", "system clock is wrong", "proxy error",
                                        "HTTPS not available", "blocked",        "cancelled",
                                        "no network"};
    return e >= 0 && e < (int)(sizeof names / sizeof names[0]) ? names[e] : "failed";
}

/* How long each phase of r took, as ", queued 10, DNS 40, connect 120". */
static void trace_phases(const AuNetRecord *r, AuBuf *out)
{
    static const char *const names[] = {"queued",  "entropy", "DNS",     "connect",
                                        "TLS",     "send",    "wait",    "receive"};
    bool first = true;
    for (int i = AU_FETCH_PHASE_QUEUED; i < AU_FETCH_PHASE_DONE; i++) {
        if (r->at[i] < 0)
            continue;
        int32_t next = -1;
        for (int k = i + 1; k <= AU_FETCH_PHASE_DONE && next < 0; k++)
            next = r->at[k];
        if (next < 0)
            next = r->total_ms;
        int32_t d = next - r->at[i];
        if (d < 1 && i != AU_FETCH_PHASE_WAITING)
            continue;
        au_buf_printf(out, "%s%s %ld", first ? ": " : ", ", names[i], (long)d);
        first = false;
    }
}

void au_netlog_trace(AuNetLog *l, uint32_t seq, uint32_t now_ms, AuBuf *out)
{
    const AuNetRecord *r = find(l, seq);
    if (!r || r->source == AU_NETREC_FROM_DATA)
        return; /* data: URLs are many and say nothing about the network */
    au_buf_printf(out, "[net] %lu #%lu ", (unsigned long)now_ms, (unsigned long)r->seq);
    switch (r->state) {
    case AU_NETREC_PENDING:
        if (r->first_ms < 0)
            au_buf_printf(out, "%s %s %s", r->method, kind_name(r), r->url);
        else
            au_buf_printf(out, "%d %s after %ld ms%s", r->status, r->type[0] ? r->type : "-", (long)r->first_ms,
                          r->source == AU_FETCH_FROM_NETWORK || r->source >= AU_NETREC_FROM_FILE
                              ? ""
                              : r->source == AU_FETCH_FROM_304 ? " (revalidated)" : " (from the cache)");
        return;
    case AU_NETREC_DONE:
        au_buf_printf(out, "done %lu bytes in %ld ms", (unsigned long)r->decoded, (long)r->total_ms);
        break;
    case AU_NETREC_FAILED:
        au_buf_printf(out, "failed (%s) after %ld ms", error_name(r->error), (long)r->total_ms);
        break;
    case AU_NETREC_BLOCKED:
        au_buf_printf(out, "blocked (%s)", r->blocked_reason);
        break;
    default:
        au_buf_printf(out, "cancelled after %ld ms", (long)r->total_ms);
        break;
    }
    if (r->have_net) {
        trace_phases(r, out);
        if (r->connection)
            au_buf_printf(out, "; connection #%lu%s %s", (unsigned long)r->connection, r->reused ? " (reused)" : "",
                          r->remote);
    }
    if (r->state != AU_NETREC_DONE)
        au_buf_printf(out, " %s", r->url);
}

/* The table's columns: title, width in cells, right-aligned. Two spaces
 * separate them; the URL takes the rest of the line. The transferred
 * size, the connection and the remote address are in the details, so
 * that the URL keeps room in a narrow window. */
static const struct {
    const char *title;
    int width;
    bool right;
} g_cols[AU_NETCOL_COUNT] = {
    {"#", 4, true},      {"Status", 7, false}, {"Method", 6, false}, {"Type", 8, false},
    {"Size", 8, true},   {"Time", 7, true},    {"From", 7, false},   {"URL", 0, false},
};
#define COL_GAP 2

static void put_cell(AuBuf *out, int col, const char *s)
{
    if (col == AU_NETCOL_URL) {
        au_buf_puts(out, s);
        return;
    }
    int w = g_cols[col].width, n = (int)strlen(s);
    if (n > w)
        n = w;
    if (g_cols[col].right)
        for (int i = n; i < w; i++)
            au_buf_putc(out, ' ');
    au_buf_append(out, s, (size_t)n);
    if (!g_cols[col].right)
        for (int i = n; i < w; i++)
            au_buf_putc(out, ' ');
    for (int i = 0; i < COL_GAP; i++)
        au_buf_putc(out, ' ');
}

int au_netlog_fixed_cells(void)
{
    int n = 0;
    for (int c = 0; c < AU_NETCOL_URL; c++)
        n += g_cols[c].width + COL_GAP;
    return n;
}

int au_netlog_column_at(int cell)
{
    int x = 0;
    if (cell < 0)
        return -1;
    for (int c = 0; c < AU_NETCOL_URL; c++) {
        x += g_cols[c].width + COL_GAP;
        if (cell < x)
            return c;
    }
    return AU_NETCOL_URL;
}

void au_netlog_header_sorted(int sort, bool descending, AuBuf *out)
{
    char t[24];
    for (int c = 0; c < AU_NETCOL_COUNT; c++) {
        /* The sorted column carries a mark: "^" rising, "v" falling. */
        const char *mark = descending ? "v" : "^";
        if (c == sort && g_cols[c].right)
            snprintf(t, sizeof t, "%s %s", mark, g_cols[c].title);
        else if (c == sort)
            snprintf(t, sizeof t, "%s %s", g_cols[c].title, mark);
        else
            snprintf(t, sizeof t, "%s", g_cols[c].title);
        put_cell(out, c, t);
    }
}

void au_netlog_header(AuBuf *out)
{
    au_netlog_header_sorted(-1, false, out);
}

const char *au_netlog_status_text(const AuNetRecord *r, char *buf, size_t cap)
{
    if (r->state == AU_NETREC_BLOCKED)
        snprintf(buf, cap, "blocked");
    else if (r->state == AU_NETREC_CANCELLED)
        snprintf(buf, cap, "stopped");
    else if (r->state == AU_NETREC_FAILED && !r->status)
        snprintf(buf, cap, "failed");
    else if (r->status)
        snprintf(buf, cap, "%d", r->status);
    else if (r->state == AU_NETREC_PENDING)
        snprintf(buf, cap, "pending");
    else
        snprintf(buf, cap, "ok");
    return buf;
}

bool au_netrec_problem(const AuNetRecord *r)
{
    return r->state == AU_NETREC_FAILED || r->state == AU_NETREC_BLOCKED || r->status >= 400;
}

void au_netlog_url_elide(const char *url, int width, AuBuf *out)
{
    /* "https://" is left out so more of the address fits; other schemes
     * (http: among them) are shown. */
    if (!strncmp(url, "https://", 8))
        url += 8;
    size_t n = strlen(url);
    if (width <= 0 || n <= (size_t)width) {
        au_buf_append(out, url, n);
        return;
    }
    if (width < 8) {
        au_buf_append(out, url, (size_t)width);
        return;
    }
    /* Keep the start (the host) and the end (the file name): two thirds
     * and one third, with "..." between. Bytes of a UTF-8 sequence are
     * not split. */
    size_t head = (size_t)(width - 3) * 2 / 3, tail = (size_t)(width - 3) - head;
    while (head && ((unsigned char)url[head] & 0xC0) == 0x80)
        head--;
    const char *t = url + n - tail;
    while (*t && ((unsigned char)*t & 0xC0) == 0x80)
        t++;
    au_buf_append(out, url, head);
    au_buf_puts(out, "...");
    au_buf_puts(out, t);
}

void au_netlog_row_url(const AuNetRecord *r, const char *url, int url_cells, AuBuf *out)
{
    char t[32];
    AuBuf b;
    au_buf_init(&b);
    snprintf(t, sizeof t, "%lu", (unsigned long)r->seq);
    put_cell(out, AU_NETCOL_SEQ, t);
    put_cell(out, AU_NETCOL_STATUS, au_netlog_status_text(r, t, sizeof t));
    put_cell(out, AU_NETCOL_METHOD, r->method);
    put_cell(out, AU_NETCOL_KIND, kind_name(r));
    /* A blocked request was never made: it has no size, time or source. */
    bool made = r->state != AU_NETREC_BLOCKED;
    if (made && (r->state != AU_NETREC_PENDING || r->decoded))
        au_put_size(&b, r->decoded);
    put_cell(out, AU_NETCOL_SIZE, au_buf_cstr(&b));
    au_buf_clear(&b);
    if (made && r->total_ms >= 0)
        au_put_ms(&b, (uint32_t)r->total_ms);
    put_cell(out, AU_NETCOL_TIME, au_buf_cstr(&b));
    put_cell(out, AU_NETCOL_FROM,
             !made ? "-" : r->state == AU_NETREC_PENDING && r->source == AU_NETREC_FROM_UNKNOWN ? "" : source_name(r->source));
    au_netlog_url_elide(url ? url : r->url, url_cells, out);
    au_buf_free(&b);
}

void au_netlog_row_fit(const AuNetRecord *r, int cells, AuBuf *out)
{
    int room = cells > 0 ? cells - au_netlog_fixed_cells() : 0;
    au_netlog_row_url(r, NULL, cells > 0 ? (room < 12 ? 12 : room) : 0, out);
}

void au_netlog_row(const AuNetRecord *r, AuBuf *out)
{
    au_netlog_row_url(r, NULL, 0, out);
}

/* ---- The view: which records are shown, in which order ---- */

void au_netview_init(AuNetView *v)
{
    memset(v, 0, sizeof *v);
    v->sort = AU_NETCOL_SEQ;
}

void au_netview_free(AuNetView *v)
{
    au_free(v->rows);
    v->rows = NULL;
    v->n = v->cap = 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]) {
            unsigned char a = (unsigned char)hay[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + 32);
            if (a != b)
                break;
            i++;
        }
        if (i == n)
            return true;
    }
    return false;
}

bool au_text_contains_ci(const char *text, const char *needle)
{
    return contains_ci(text, needle);
}

static bool view_shows(const AuNetView *v, const AuNetRecord *r)
{
    if (v->show == AU_NETSHOW_PROBLEMS && !au_netrec_problem(r))
        return false;
    if (v->show == AU_NETSHOW_BLOCKED && r->state != AU_NETREC_BLOCKED)
        return false;
    if (v->filter[0]) {
        char st[16];
        /* The filter matches the address, the type, the status and the
         * reason a request was blocked. */
        if (!contains_ci(r->url, v->filter) && !contains_ci(kind_name(r), v->filter) &&
            !contains_ci(au_netlog_status_text(r, st, sizeof st), v->filter) &&
            !contains_ci(r->blocked_reason, v->filter) && !contains_ci(r->type, v->filter))
            return false;
    }
    return true;
}

static int cmp_rec(const AuNetRecord *a, const AuNetRecord *b, int col)
{
    switch (col) {
    case AU_NETCOL_STATUS: {
        /* Problems first when rising: blocked, failed, then by number. */
        int sa = a->state == AU_NETREC_BLOCKED ? -2 : a->state == AU_NETREC_FAILED && !a->status ? -1 : a->status;
        int sb = b->state == AU_NETREC_BLOCKED ? -2 : b->state == AU_NETREC_FAILED && !b->status ? -1 : b->status;
        return sa < sb ? -1 : sa > sb;
    }
    case AU_NETCOL_METHOD: return strcmp(a->method, b->method);
    case AU_NETCOL_KIND: return strcmp(kind_name(a), kind_name(b));
    case AU_NETCOL_SIZE: return a->decoded < b->decoded ? -1 : a->decoded > b->decoded;
    case AU_NETCOL_TIME: return a->total_ms < b->total_ms ? -1 : a->total_ms > b->total_ms;
    case AU_NETCOL_FROM: return strcmp(source_name(a->source), source_name(b->source));
    case AU_NETCOL_URL: return strcmp(a->url, b->url);
    default: return 0;
    }
}

void au_netview_build(AuNetView *v, const AuNetLog *l)
{
    v->n = 0;
    if (!l)
        return;
    if (v->cap < l->count) {
        v->rows = au_realloc(v->rows, (size_t)l->count * sizeof *v->rows);
        v->cap = l->count;
    }
    for (uint32_t i = 0; i < l->count; i++)
        if (view_shows(v, au_netlog_at(l, i)))
            v->rows[v->n++] = i;
    if (v->sort > AU_NETCOL_SEQ && v->sort < AU_NETCOL_COUNT) {
        /* An insertion sort: at most AU_NETLOG_MAX rows, and equal rows
         * keep the order the requests started in. */
        for (uint32_t i = 1; i < v->n; i++) {
            uint32_t x = v->rows[i], k = i;
            const AuNetRecord *rx = au_netlog_at(l, x);
            while (k > 0) {
                int c = cmp_rec(au_netlog_at(l, v->rows[k - 1]), rx, v->sort);
                if (v->desc ? c >= 0 : c <= 0)
                    break;
                v->rows[k] = v->rows[k - 1];
                k--;
            }
            v->rows[k] = x;
        }
    } else if (v->desc) {
        for (uint32_t i = 0; i < v->n / 2; i++) {
            uint32_t x = v->rows[i];
            v->rows[i] = v->rows[v->n - 1 - i];
            v->rows[v->n - 1 - i] = x;
        }
    }
}

void au_netview_click_column(AuNetView *v, int col)
{
    if (col < 0 || col >= AU_NETCOL_COUNT)
        return;
    if (v->sort == col) {
        v->desc = !v->desc;
    } else {
        v->sort = col;
        /* Sizes and times are looked at largest first. */
        v->desc = col == AU_NETCOL_SIZE || col == AU_NETCOL_TIME;
    }
}

/* The time spent in each phase: from its stamp to the next phase that
 * has one. */
static void put_timings(const AuNetRecord *r, AuBuf *out)
{
    static const char *const names[] = {"queued", "entropy", "DNS", "connect", "TLS", "request sent",
                                        "waiting (first byte)", "receiving"};
    bool any = false;
    for (int i = 0; i < AU_FETCH_PHASE_DONE; i++) {
        if (r->at[i] < 0)
            continue;
        int32_t next = -1;
        for (int k = i + 1; k <= AU_FETCH_PHASE_DONE && next < 0; k++)
            next = r->at[k];
        if (next < 0)
            next = r->total_ms;
        if (next < r->at[i])
            continue;
        au_buf_printf(out, "%s%s ", any ? ", " : "", names[i]);
        au_put_ms(out, (uint32_t)(next - r->at[i]));
        any = true;
    }
    if (!any)
        au_buf_puts(out, "not recorded");
}

static void put_head(AuBuf *out, const char *title, const char *head)
{
    au_buf_printf(out, "%s\n", title);
    const char *s = head;
    while (*s) {
        size_t n = strcspn(s, "\r\n");
        if (n) {
            au_buf_puts(out, "  ");
            au_buf_append(out, s, n);
            au_buf_putc(out, '\n');
        }
        s += n;
        while (*s == '\r' || *s == '\n')
            s++;
    }
}

void au_netlog_detail(const AuNetRecord *r, AuBuf *out)
{
    AuBuf v;
    au_buf_init(&v);
    au_buf_printf(out, "Request %lu\n", (unsigned long)r->seq);
    au_report_kv(out, "URL", "%s", r->url);
    au_report_kv(out, "Method and type", "%s, %s", r->method, kind_name(r));
    if (r->status)
        au_buf_printf(&v, "%d", r->status);
    else
        au_buf_puts(&v, r->state == AU_NETREC_PENDING   ? "pending"
                        : r->state == AU_NETREC_BLOCKED ? "blocked: the request was never made"
                                                        : "none");
    if (r->type[0])
        au_buf_printf(&v, ", %s", r->type);
    if (r->redirects)
        au_buf_printf(&v, ", after %d redirect%s", r->redirects, r->redirects == 1 ? "" : "s");
    au_report_kv(out, "Status", "%s", au_buf_cstr(&v));
    if (r->state == AU_NETREC_BLOCKED) {
        /* A blocklist's reason is "blocked: LIST NAME". */
        const char *why = r->blocked_reason[0] ? r->blocked_reason : "policy";
        au_report_kv(out, "Blocked by", "%s", !strncmp(why, "blocked: ", 9) ? why + 9 : why);
        au_buf_clear(&v);
        au_put_ms(&v, r->start_ms);
        au_report_kv(out, "Asked for", "%s after the navigation", au_buf_cstr(&v));
        au_buf_free(&v);
        return;
    }
    if (r->state == AU_NETREC_FAILED)
        au_report_kv(out, "Failed", "%s", error_name(r->error));
    else if (r->state == AU_NETREC_CANCELLED)
        au_report_kv(out, "Stopped", "before it finished");
    au_buf_clear(&v);
    au_buf_puts(&v, source_name(r->source));
    if (r->connection)
        au_buf_printf(&v, ", connection #%lu (%s)", (unsigned long)r->connection,
                      r->reused ? "kept open from an earlier request" : "new");
    if (r->remote[0])
        au_buf_printf(&v, ", %s", r->remote);
    au_report_kv(out, "From", "%s", au_buf_cstr(&v));
    if (r->protocol[0])
        au_report_kv(out, "Security", "%s, %s%s", r->protocol, r->cipher[0] ? r->cipher : "cipher not recorded",
                     !strncmp(r->cipher, "RSA-", 4) ? ", RSA key exchange (no forward secrecy)" : "");
    au_buf_clear(&v);
    if (r->have_net && r->source == AU_FETCH_FROM_NETWORK) {
        au_put_size(&v, r->transfer);
        au_buf_puts(&v, " transferred, ");
    }
    au_put_size(&v, r->decoded);
    au_buf_puts(&v, " decoded");
    au_report_kv(out, "Size", "%s", au_buf_cstr(&v));
    au_buf_clear(&v);
    au_put_ms(&v, r->start_ms);
    au_buf_puts(&v, " after the navigation");
    if (r->first_ms >= 0) {
        au_buf_puts(&v, "; response after ");
        au_put_ms(&v, (uint32_t)r->first_ms);
    }
    if (r->total_ms >= 0) {
        au_buf_puts(&v, "; done after ");
        au_put_ms(&v, (uint32_t)r->total_ms);
    }
    au_report_kv(out, "Started", "%s", au_buf_cstr(&v));
    au_buf_clear(&v);
    put_timings(r, &v);
    au_report_kv(out, "Time in each phase", "%s", au_buf_cstr(&v));
    au_buf_free(&v);
    if (r->request_head)
        put_head(out, "Request headers", r->request_head);
    if (r->response_head)
        put_head(out, "Response headers", r->response_head);
    if (!r->request_head && !r->response_head && r->have_net && r->source == AU_FETCH_FROM_NETWORK)
        au_report_kv(out, "Headers", "not recorded (reload with the Network window open)");
}

void au_netlog_footer(const AuNetLog *l, const char *connections, AuBuf *out)
{
    au_buf_printf(out, "%lu request%s", (unsigned long)l->requests, l->requests == 1 ? "" : "s");
    if (l->requests > l->finished)
        au_buf_printf(out, " (%lu pending)", (unsigned long)(l->requests - l->finished));
    au_buf_puts(out, ", ");
    au_put_size(out, l->transfer_bytes);
    au_buf_puts(out, " transferred, ");
    au_put_size(out, l->decoded_bytes);
    au_buf_puts(out, " decoded");
    if (l->from_cache)
        au_buf_printf(out, ", %lu from cache", (unsigned long)l->from_cache);
    if (l->blocked)
        au_buf_printf(out, ", %lu blocked", (unsigned long)l->blocked);
    if (l->failed)
        au_buf_printf(out, ", %lu failed", (unsigned long)l->failed);
    if (l->dcl_ms) {
        au_buf_puts(out, "; DOMContentLoaded ");
        au_put_ms(out, l->dcl_ms);
    }
    if (l->load_ms) {
        au_buf_puts(out, "; load ");
        au_put_ms(out, l->load_ms);
    }
    if (connections && *connections)
        au_buf_printf(out, "; connections: %s", connections);
}

void au_netlog_blocked(const AuNetLog *l, AuBuf *out)
{
    if (!l->blocked)
        return;
    au_buf_printf(out, "Blocked requests (%lu)\n", (unsigned long)l->blocked);
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        if (r->state != AU_NETREC_BLOCKED)
            continue;
        const char *why = r->blocked_reason[0] ? r->blocked_reason : "by policy";
        if (!strncmp(why, "blocked: ", 9))
            why += 9; /* a blocklist's reason is "blocked: LIST NAME" */
        au_buf_printf(out, "  %s  (%s)\n", r->url, why);
    }
}

void au_netlog_dump(const AuNetLog *l, const char *connections, bool details, AuBuf *out)
{
    au_netlog_header(out);
    au_buf_putc(out, '\n');
    if (l->requests > l->count)
        au_buf_printf(out, "(%lu earlier requests are no longer kept)\n", (unsigned long)(l->requests - l->count));
    AuBuf d;
    au_buf_init(&d);
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        au_netlog_row(r, out);
        au_buf_putc(out, '\n');
        if (!details)
            continue;
        au_buf_clear(&d);
        au_netlog_detail(r, &d);
        const char *s = au_buf_cstr(&d);
        while (*s) {
            size_t n = strcspn(s, "\n");
            au_buf_puts(out, "        ");
            au_buf_append(out, s, n);
            au_buf_putc(out, '\n');
            s += n + (s[n] == '\n');
        }
    }
    au_buf_free(&d);
    au_buf_putc(out, '\n');
    au_netlog_summary(l, connections, out);
}

void au_netlog_summary(const AuNetLog *l, const char *connections, AuBuf *out)
{
    AuBuf v;
    au_buf_init(&v);
    au_buf_puts(out, "Summary\n");
    au_put_count(&v, l->requests);
    if (l->requests > l->finished)
        au_buf_printf(&v, " (%lu pending)", (unsigned long)(l->requests - l->finished));
    au_buf_printf(&v, "   (%lu from cache, %lu blocked, %lu failed)", (unsigned long)l->from_cache,
                  (unsigned long)l->blocked, (unsigned long)l->failed);
    au_report_kv(out, "Requests", "%s", au_buf_cstr(&v));
    au_buf_clear(&v);
    au_put_size(&v, l->transfer_bytes);
    au_buf_puts(&v, " transferred, ");
    au_put_size(&v, l->decoded_bytes);
    au_buf_puts(&v, " decoded");
    au_report_kv(out, "Size", "%s", au_buf_cstr(&v));
    au_buf_clear(&v);
    if (l->dcl_ms)
        au_put_ms(&v, l->dcl_ms);
    else
        au_buf_puts(&v, "not yet");
    au_report_kv(out, "DOMContentLoaded", "%s", au_buf_cstr(&v));
    au_buf_clear(&v);
    if (l->load_ms)
        au_put_ms(&v, l->load_ms);
    else
        au_buf_puts(&v, "not yet");
    au_report_kv(out, "Load", "%s", au_buf_cstr(&v));
    au_buf_free(&v);
    if (connections && *connections)
        au_report_kv(out, "Connections", "%s", connections);
    /* One line per request that failed, was blocked or was redirected. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        if (au_netrec_problem(r) || r->redirects)
            n++;
    }
    au_buf_printf(out, "Failed, blocked and redirected requests (%lu)\n", (unsigned long)n);
    if (!n)
        au_buf_puts(out, "  none\n");
    for (uint32_t i = 0; i < l->count; i++) {
        const AuNetRecord *r = au_netlog_at(l, i);
        if (!au_netrec_problem(r) && !r->redirects)
            continue;
        au_netlog_problem_line(r, NULL, out);
        au_buf_putc(out, '\n');
    }
}

void au_netlog_problem_line(const AuNetRecord *r, const char *url, AuBuf *out)
{
    char st[16];
    bool problem = au_netrec_problem(r);
    au_buf_printf(out, "  %-9s", problem ? au_netlog_status_text(r, st, sizeof st) : "redirect");
    au_buf_printf(out, " %s", url ? url : r->url);
    if (r->state == AU_NETREC_BLOCKED) {
        const char *why = r->blocked_reason[0] ? r->blocked_reason : "by policy";
        if (!strncmp(why, "blocked: ", 9))
            why += 9; /* a blocklist's reason is "blocked: LIST NAME" */
        au_buf_printf(out, "  (%s)", why);
    } else if (r->state == AU_NETREC_FAILED && !r->status) {
        au_buf_printf(out, "  (%s)", error_name(r->error));
    }
    if (r->redirects)
        au_buf_printf(out, "  (after %d redirect%s)", r->redirects, r->redirects == 1 ? "" : "s");
}
