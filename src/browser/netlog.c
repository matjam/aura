/* The request log of a page (see netlog.h). */
#include "netlog.h"

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
    if (l->recs)
        for (uint32_t i = 0; i < AU_NETLOG_MAX; i++)
            if (l->recs[i].url)
                rec_free(&l->recs[i]);
    uint32_t gen = l->generation + 1;
    AuNetRecord *recs = l->recs;
    memset(l, 0, sizeof *l);
    l->recs = recs;
    l->first_seq = 1;
    l->generation = gen;
}

void au_netlog_free(AuNetLog *l)
{
    au_netlog_clear(l);
    au_free(l->recs);
    l->recs = NULL;
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
    if (!l->recs)
        l->recs = au_calloc(AU_NETLOG_MAX * sizeof *l->recs);
    uint32_t seq = ++l->requests;
    AuNetRecord *r = &l->recs[(seq - 1) % AU_NETLOG_MAX];
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

/* What the request was for. The document view asks for scripts, fonts
 * and script fetches as "other"; the response's type says which. */
static const char *kind_name(const AuNetRecord *r)
{
    static const char *const names[] = {"document", "style", "image", "font", "script", "other"};
    int k = r->kind;
    if (k == AU_FETCH_OTHER) {
        if (strstr(r->type, "javascript") || strstr(r->type, "ecmascript"))
            return "script";
        if (!strncmp(r->type, "font/", 5) || strstr(r->type, "font-"))
            return "font";
        if (strstr(r->type, "json"))
            return "json";
    }
    return k >= 0 && k < 6 ? names[k] : "other";
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

/* Appends s padded with spaces to width columns (s is ASCII). */
static void pad(AuBuf *out, const char *s, int width)
{
    int n = (int)strlen(s);
    au_buf_puts(out, s);
    for (; n < width; n++)
        au_buf_putc(out, ' ');
}

static void pad_right(AuBuf *out, const char *s, int width)
{
    for (int n = (int)strlen(s); n < width; n++)
        au_buf_putc(out, ' ');
    au_buf_puts(out, s);
}

/* Column widths: #, status, method, kind, transferred, size, time, from,
 * connection. The URL takes the rest. */
enum { W_SEQ = 4, W_STATUS = 6, W_METHOD = 5, W_KIND = 8, W_XFER = 9, W_SIZE = 9, W_TIME = 8, W_FROM = 8, W_CONN = 9 };

void au_netlog_header(AuBuf *out)
{
    pad_right(out, "#", W_SEQ);
    au_buf_puts(out, "  ");
    pad(out, "Status", W_STATUS + 1);
    pad(out, "Meth", W_METHOD + 1);
    pad(out, "Type", W_KIND + 1);
    pad_right(out, "Transfer", W_XFER);
    pad_right(out, "Size", W_SIZE + 1);
    pad_right(out, "Time", W_TIME + 1);
    au_buf_puts(out, "  ");
    pad(out, "From", W_FROM + 1);
    pad(out, "Conn", W_CONN + 1);
    au_buf_puts(out, "URL");
}

void au_netlog_row(const AuNetRecord *r, AuBuf *out)
{
    char t[32];
    AuBuf b;
    au_buf_init(&b);
    snprintf(t, sizeof t, "%lu", (unsigned long)r->seq);
    pad_right(out, t, W_SEQ);
    au_buf_puts(out, "  ");
    if (r->state == AU_NETREC_BLOCKED)
        snprintf(t, sizeof t, "block");
    else if (r->state == AU_NETREC_CANCELLED)
        snprintf(t, sizeof t, "stop");
    else if (r->state == AU_NETREC_FAILED && !r->status)
        snprintf(t, sizeof t, "fail");
    else if (r->status)
        snprintf(t, sizeof t, "%d", r->status);
    else if (r->state == AU_NETREC_PENDING)
        snprintf(t, sizeof t, "...");
    else
        snprintf(t, sizeof t, "ok");
    pad(out, t, W_STATUS + 1);
    pad(out, r->method, W_METHOD + 1);
    pad(out, kind_name(r), W_KIND + 1);
    if (r->have_net && r->source == AU_FETCH_FROM_NETWORK)
        au_put_size(&b, r->transfer);
    else
        au_buf_puts(&b, r->state == AU_NETREC_PENDING ? "" : "-");
    pad_right(out, au_buf_cstr(&b), W_XFER);
    au_buf_clear(&b);
    au_put_size(&b, r->decoded);
    pad_right(out, au_buf_cstr(&b), W_SIZE + 1);
    au_buf_clear(&b);
    if (r->total_ms >= 0)
        au_put_ms(&b, (uint32_t)r->total_ms);
    pad_right(out, au_buf_cstr(&b), W_TIME + 1);
    au_buf_puts(out, "  ");
    pad(out, r->state == AU_NETREC_PENDING && r->source == AU_NETREC_FROM_UNKNOWN ? "" : source_name(r->source),
        W_FROM + 1);
    if (r->connection)
        snprintf(t, sizeof t, "#%lu %s", (unsigned long)r->connection, r->reused ? "kept" : "new");
    else
        snprintf(t, sizeof t, "-");
    pad(out, t, W_CONN + 1);
    /* "https://" is left out so more of the address fits a narrow window;
     * other schemes (http: among them) are shown. */
    au_buf_puts(out, !strncmp(r->url, "https://", 8) ? r->url + 8 : r->url);
    au_buf_free(&b);
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
    au_buf_printf(out, "%s:\n", title);
    const char *s = head;
    while (*s) {
        size_t n = strcspn(s, "\r\n");
        if (n) {
            au_buf_puts(out, "    ");
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
    au_buf_printf(out, "URL: %s\n", r->url);
    au_buf_printf(out, "Request: %s, %s\n", r->method, kind_name(r));
    au_buf_puts(out, "Status: ");
    if (r->status)
        au_buf_printf(out, "%d", r->status);
    else
        au_buf_puts(out, r->state == AU_NETREC_PENDING ? "pending" : "none");
    if (r->type[0])
        au_buf_printf(out, ", %s", r->type);
    if (r->redirects)
        au_buf_printf(out, ", after %d redirect%s", r->redirects, r->redirects == 1 ? "" : "s");
    au_buf_putc(out, '\n');
    if (r->state == AU_NETREC_BLOCKED)
        au_buf_printf(out, "Blocked: %s\n", r->blocked_reason[0] ? r->blocked_reason : "by policy");
    else if (r->state == AU_NETREC_FAILED)
        au_buf_printf(out, "Failed: %s\n", error_name(r->error));
    else if (r->state == AU_NETREC_CANCELLED)
        au_buf_puts(out, "Stopped before it finished\n");
    au_buf_printf(out, "From: %s", source_name(r->source));
    if (r->connection)
        au_buf_printf(out, ", connection #%lu (%s)", (unsigned long)r->connection,
                      r->reused ? "kept open from an earlier request" : "new");
    if (r->remote[0])
        au_buf_printf(out, ", %s", r->remote);
    au_buf_putc(out, '\n');
    if (r->protocol[0])
        au_buf_printf(out, "Security: %s, %s\n", r->protocol, r->cipher[0] ? r->cipher : "cipher not recorded");
    au_buf_puts(out, "Size: ");
    if (r->have_net && r->source == AU_FETCH_FROM_NETWORK) {
        au_put_size(out, r->transfer);
        au_buf_puts(out, " transferred, ");
    }
    au_put_size(out, r->decoded);
    au_buf_puts(out, " decoded\n");
    au_buf_puts(out, "Started: ");
    au_put_ms(out, r->start_ms);
    au_buf_puts(out, " after the navigation");
    if (r->first_ms >= 0) {
        au_buf_puts(out, "; response after ");
        au_put_ms(out, (uint32_t)r->first_ms);
    }
    if (r->total_ms >= 0) {
        au_buf_puts(out, "; done after ");
        au_put_ms(out, (uint32_t)r->total_ms);
    }
    au_buf_puts(out, "\nTimings: ");
    put_timings(r, out);
    au_buf_putc(out, '\n');
    if (r->request_head)
        put_head(out, "Request headers", r->request_head);
    if (r->response_head)
        put_head(out, "Response headers", r->response_head);
    if (!r->request_head && !r->response_head && r->have_net && r->source == AU_FETCH_FROM_NETWORK)
        au_buf_puts(out, "Headers: not recorded (reload with the Network window open)\n");
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
    au_netlog_footer(l, connections, out);
    au_buf_putc(out, '\n');
}
