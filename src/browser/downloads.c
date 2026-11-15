/* Downloads. See downloads.h. */
#include "downloads.h"
#include "chrome.h"
#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup(const char *s)
{
    return au_strndup(s ? s : "", s ? strlen(s) : 0);
}

static void set(char **f, const char *s)
{
    char *v = dup(s);
    au_free(*f);
    *f = v;
}

static char *clean(const char *s)
{
    char *c = dup(s);
    for (char *p = c; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
    return c;
}

void au_downloads_init(AuDownloads *dl, const AuTransferSource *src)
{
    memset(dl, 0, sizeof *dl);
    dl->next_id = 1;
    dl->max_active = 3;
    if (src)
        dl->src = *src;
}

static void stop_transfer(AuDownloads *dl, AuDownload *d)
{
    if (d->transfer && dl->src.stop)
        dl->src.stop(dl->src.ctx, d->transfer);
    d->transfer = NULL;
}

static void free_item(AuDownload *d)
{
    au_free(d->url);
    au_free(d->filename);
    au_free(d->mime);
    au_free(d->error);
    au_free(d);
}

void au_downloads_free(AuDownloads *dl)
{
    for (int i = 0; i < dl->n; i++) {
        stop_transfer(dl, dl->items[i]);
        free_item(dl->items[i]);
    }
    au_free(dl->items);
    memset(dl, 0, sizeof *dl);
}

static AuDownload *append(AuDownloads *dl)
{
    if (dl->n == dl->cap) {
        dl->cap = dl->cap ? dl->cap * 2 : 8;
        dl->items = au_realloc(dl->items, (size_t)dl->cap * sizeof *dl->items);
    }
    AuDownload *d = au_calloc(sizeof *d);
    dl->items[dl->n++] = d;
    return d;
}

AuDownload *au_downloads_add(AuDownloads *dl, const char *url, const char *filename, const char *mime,
                             int64_t total, AuTime now)
{
    AuDownload *d = append(dl);
    char name[128];
    au_hfs_sanitize(filename, NULL, NULL, name, sizeof name);
    d->id = dl->next_id++;
    d->url = clean(url);
    d->filename = clean(name);
    d->mime = clean(mime);
    d->error = dup("");
    d->total = total < 0 ? -1 : total;
    d->state = AU_DL_QUEUED;
    d->started = now;
    dl->generation++;
    return d;
}

AuDownload *au_downloads_find(AuDownloads *dl, uint32_t id)
{
    for (int i = 0; i < dl->n; i++)
        if (dl->items[i]->id == id)
            return dl->items[i];
    return NULL;
}

void au_download_progress(AuDownloads *dl, AuDownload *d, int64_t received, int64_t total)
{
    if (total >= 0)
        d->total = total;
    if (received < 0)
        received = 0;
    if (d->total >= 0 && received > d->total)
        received = d->total;
    d->received = received;
    dl->generation++;
}

void au_download_pause(AuDownloads *dl, AuDownload *d)
{
    if (d->state != AU_DL_ACTIVE && d->state != AU_DL_QUEUED)
        return;
    stop_transfer(dl, d);
    d->state = AU_DL_PAUSED;
    d->rate = 0;
    dl->generation++;
}

void au_download_resume(AuDownloads *dl, AuDownload *d)
{
    if (d->state != AU_DL_PAUSED && d->state != AU_DL_FAILED)
        return;
    d->state = AU_DL_QUEUED;
    d->interrupted = false;
    set(&d->error, "");
    dl->generation++;
}

void au_download_cancel(AuDownloads *dl, AuDownload *d)
{
    if (d->state == AU_DL_DONE || d->state == AU_DL_CANCELLED)
        return;
    stop_transfer(dl, d);
    d->state = AU_DL_CANCELLED;
    d->rate = 0;
    dl->generation++;
}

void au_download_remove(AuDownloads *dl, AuDownload *d)
{
    for (int i = 0; i < dl->n; i++) {
        if (dl->items[i] == d) {
            stop_transfer(dl, d);
            free_item(d);
            memmove(&dl->items[i], &dl->items[i + 1], (size_t)(dl->n - i - 1) * sizeof *dl->items);
            dl->n--;
            dl->generation++;
            return;
        }
    }
}

void au_downloads_clear_completed(AuDownloads *dl)
{
    int k = 0;
    for (int i = 0; i < dl->n; i++) {
        AuDownload *d = dl->items[i];
        if (d->state == AU_DL_DONE || d->state == AU_DL_FAILED || d->state == AU_DL_CANCELLED)
            free_item(d);
        else
            dl->items[k++] = d;
    }
    if (k != dl->n)
        dl->generation++;
    dl->n = k;
}

bool au_downloads_busy(const AuDownloads *dl)
{
    for (int i = 0; i < dl->n; i++)
        if (dl->items[i]->state == AU_DL_ACTIVE || dl->items[i]->state == AU_DL_QUEUED)
            return true;
    return false;
}

static void fail(AuDownloads *dl, AuDownload *d, char *error)
{
    stop_transfer(dl, d);
    d->state = AU_DL_FAILED;
    set(&d->error, error ? error : "Failed");
    au_free(error);
    d->rate = 0;
    dl->generation++;
}

void au_downloads_idle(AuDownloads *dl, uint32_t now_ms)
{
    int active = 0;
    for (int i = 0; i < dl->n; i++)
        active += dl->items[i]->state == AU_DL_ACTIVE;
    for (int i = 0; i < dl->n; i++) {
        AuDownload *d = dl->items[i];
        if (d->state == AU_DL_QUEUED && active < dl->max_active && dl->src.start) {
            char *err = NULL;
            d->transfer = dl->src.start(dl->src.ctx, d, d->received, &err);
            if (!d->transfer) {
                fail(dl, d, err);
                continue;
            }
            d->state = AU_DL_ACTIVE;
            d->rate_ms = now_ms;
            d->rate_bytes = d->received;
            d->rate = 0;
            active++;
            dl->generation++;
        }
        if (d->state != AU_DL_ACTIVE || !d->transfer || !dl->src.poll)
            continue;
        int64_t total = d->total;
        bool done = false;
        char *err = NULL;
        int64_t got = dl->src.poll(dl->src.ctx, d->transfer, now_ms, &total, &done, &err);
        if (err) {
            d->received = got > d->received ? got : d->received;
            fail(dl, d, err);
            continue;
        }
        if (got != d->received || total != d->total)
            au_download_progress(dl, d, got, total);
        /* Rate: a sample every half second, smoothed. */
        uint32_t dt = now_ms - d->rate_ms;
        if (dt >= 500) {
            uint32_t r = (uint32_t)((d->received - d->rate_bytes) * 1000 / (int64_t)dt);
            d->rate = d->rate ? (d->rate * 3 + r) / 4 : r;
            d->rate_ms = now_ms;
            d->rate_bytes = d->received;
            dl->generation++;
        }
        if (done) {
            stop_transfer(dl, d);
            if (d->total < 0)
                d->total = d->received;
            d->state = AU_DL_DONE;
            d->rate = 0;
            dl->generation++;
        }
    }
}

long au_download_eta(const AuDownload *d)
{
    if (d->state != AU_DL_ACTIVE || d->total < 0 || d->rate == 0)
        return -1;
    return (long)((d->total - d->received + d->rate - 1) / d->rate);
}

static const char *const state_names[] = {"queued", "active", "paused", "done", "failed", "cancelled"};

const char *au_download_state_name(AuDownloadState s)
{
    return (unsigned)s < 6 ? state_names[s] : "failed";
}

void au_format_size(int64_t b, char *out, size_t cap)
{
    if (b < 1024)
        snprintf(out, cap, "%d bytes", (int)b);
    else if (b < 1024 * 1024)
        snprintf(out, cap, "%d KB", (int)((b + 512) / 1024));
    else
        snprintf(out, cap, "%d.%d MB", (int)(b / (1024 * 1024)), (int)(b % (1024 * 1024) * 10 / (1024 * 1024)));
}

/* ---- File ---- */

void au_downloads_serialize(const AuDownloads *dl, AuBuf *out)
{
    au_buf_puts(out, "# Aura downloads 1\n");
    for (int i = 0; i < dl->n; i++) {
        const AuDownload *d = dl->items[i];
        AuDownloadState s = d->state;
        const char *err = d->error;
        if (s == AU_DL_ACTIVE || s == AU_DL_QUEUED) {
            s = AU_DL_PAUSED;
            err = "Interrupted";
        }
        au_buf_printf(out, "%lu\t%s\t", (unsigned long)d->id, state_names[s]);
        au_put_i64(out, d->received);
        au_buf_putc(out, '\t');
        au_put_i64(out, d->total);
        au_buf_putc(out, '\t');
        au_put_i64(out, d->started);
        au_buf_printf(out, "\t%s\t%s\t%s\t%s\n", d->url, d->filename, d->mime, err);
    }
}

static bool num(const char *s, size_t n, int64_t *out)
{
    if (n == 0 || n > 19)
        return false;
    size_t i = 0;
    bool neg = s[0] == '-';
    if (neg && n == 1)
        return false;
    int64_t v = 0;
    for (i = neg; i < n; i++) {
        if (!au_is_digit((unsigned char)s[i]))
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

bool au_downloads_parse(AuDownloads *dl, const char *data, size_t len)
{
    AuTransferSource src = dl->src;
    int max_active = dl->max_active;
    au_downloads_free(dl);
    au_downloads_init(dl, &src);
    dl->max_active = max_active;
    size_t good = 0, bad = 0, i = 0;
    bool header = false;
    while (i < len) {
        size_t e = i;
        while (e < len && data[e] != '\n' && data[e] != '\r')
            e++;
        const char *line = data + i;
        size_t n = e - i;
        i = e;
        while (i < len && (data[i] == '\n' || data[i] == '\r'))
            i++;
        if (n == 0)
            continue;
        if (line[0] == '#') {
            if (n >= 18 && !strncmp(line, "# Aura downloads 1", 18))
                header = true;
            continue;
        }
        const char *f[9];
        size_t fl[9];
        int nf = 0;
        size_t s = 0;
        for (size_t k = 0; k <= n && nf < 9; k++) {
            if (k == n || (line[k] == '\t' && nf < 8)) {
                f[nf] = line + s;
                fl[nf++] = k - s;
                s = k + 1;
            }
        }
        int64_t id, rec, tot, started;
        int st = -1;
        if (nf == 9) {
            for (int k = 0; k < 6; k++)
                if (au_ieq(f[1], fl[1], state_names[k], strlen(state_names[k])))
                    st = k;
        }
        if (nf < 9 || memchr(line, 0, n) || st < 0 || !num(f[0], fl[0], &id) || !num(f[2], fl[2], &rec) ||
            !num(f[3], fl[3], &tot) || !num(f[4], fl[4], &started) || fl[5] == 0 || id <= 0 || rec < 0) {
            bad++;
            continue;
        }
        good++;
        AuDownload *d = append(dl);
        d->id = (uint32_t)id;
        if (d->id >= dl->next_id)
            dl->next_id = d->id + 1;
        d->state = (AuDownloadState)st;
        d->received = rec;
        d->total = tot < 0 ? -1 : tot;
        d->started = started;
        d->url = au_strndup(f[5], fl[5]);
        d->filename = au_strndup(f[6], fl[6]);
        d->mime = au_strndup(f[7], fl[7]);
        d->error = au_strndup(f[8], fl[8]);
        if (d->state == AU_DL_ACTIVE || d->state == AU_DL_QUEUED) {
            d->state = AU_DL_PAUSED;
            set(&d->error, "Interrupted");
        }
        d->interrupted = d->state == AU_DL_PAUSED && !strcmp(d->error, "Interrupted");
    }
    dl->generation++;
    if (len > 0 && !header)
        return false;
    return good >= bad;
}

/* ---- HFS names ---- */

static size_t chars(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

/* The byte length of the first max characters of s. */
static size_t prefix_bytes(const char *s, size_t n, size_t max)
{
    size_t c = 0, i = 0;
    while (i < n) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (c == max)
                break;
            c++;
        }
        i++;
    }
    return i;
}

static void compose(char *out, size_t cap, const char *base, size_t bn, const char *suffix, const char *ext,
                    size_t en)
{
    size_t sn = strlen(suffix);
    size_t room = 31 - chars(suffix, sn) - chars(ext, en);
    size_t bb = prefix_bytes(base, bn, room);
    while (bb > 0 && base[bb - 1] == ' ')
        bb--;
    snprintf(out, cap, "%.*s%s%.*s", (int)bb, base, suffix, (int)en, ext);
}

void au_hfs_sanitize(const char *name, bool (*exists)(void *ctx, const char *name), void *ctx, char *out,
                     size_t cap)
{
    AuBuf b;
    au_buf_init(&b);
    for (const char *p = name ? name : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F)
            continue;
        au_buf_putc(&b, c == ':' ? '-' : (char)c);
    }
    au_buf_cstr(&b);
    /* Trim spaces at both ends. */
    char *s = b.data;
    size_t n = b.len;
    while (n && *s == ' ')
        s++, n--;
    while (n && s[n - 1] == ' ')
        n--;
    if (n == 0) {
        s = "download";
        n = 8;
    }
    char first = s[0];
    /* Split off the extension: the last '.', not leading, at most 8. */
    size_t en = 0;
    for (size_t i = n; i-- > 1;) {
        if (s[i] == '.') {
            if (n - i <= 8)
                en = n - i;
            break;
        }
    }
    const char *ext = s + n - en;
    size_t bn = n - en;
    char base[256];
    if (bn >= sizeof base)
        bn = sizeof base - 1;
    memcpy(base, s, bn);
    base[bn] = 0;
    if (first == '.')
        base[0] = '_';
    char exts[16];
    memcpy(exts, ext, en);
    exts[en] = 0;
    compose(out, cap, base, bn, "", exts, en);
    for (int k = 2; exists && exists(ctx, out) && k < 10000; k++) {
        char suf[16];
        snprintf(suf, sizeof suf, " %d", k);
        compose(out, cap, base, bn, suf, exts, en);
    }
    au_buf_free(&b);
}

void au_download_name_from_url(const char *url, char *out, size_t cap)
{
    size_t n = strcspn(url, "?#");
    const char *path = url;
    size_t k = au_url_scheme_len(url, n);
    if (k && !strncmp(url + k + 1, "//", 2)) {
        const char *h = url + k + 3;
        const char *slash = memchr(h, '/', n - (size_t)(h - url));
        path = slash ? slash : url + n;
    }
    const char *end = url + n;
    const char *seg = end;
    while (seg > path && seg[-1] != '/')
        seg--;
    AuBuf b;
    au_buf_init(&b);
    if (seg < end)
        au_url_percent_decode(&b, seg, (size_t)(end - seg));
    if (b.len == 0) {
        char host[128];
        au_url_host(url, host, sizeof host);
        au_buf_puts(&b, host[0] ? host : "download");
    }
    snprintf(out, cap, "%s", au_buf_cstr(&b));
    au_buf_free(&b);
}

/* ---- Simulated source ---- */

typedef struct Sim {
    int64_t size, offset;
    bool hide_size, fail;
    uint32_t rate; /* bytes per second */
    uint32_t start_ms;
    bool started;
} Sim;

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void *sim_start(void *ctx, AuDownload *d, int64_t offset, char **error)
{
    (void)ctx;
    if (strstr(d->url, "refuse")) {
        *error = au_strndup("Connection refused", 18);
        return NULL;
    }
    Sim *s = au_calloc(sizeof *s);
    uint32_t h = hash(d->url);
    s->size = d->total > 0 ? d->total : 40 * 1024 + (int64_t)(h % (3 * 1024 * 1024));
    s->hide_size = d->total < 0 && (h & 4);
    s->fail = strstr(d->url, "fail") != NULL;
    s->rate = 20 * 1024 + (h >> 8) % (100 * 1024);
    s->offset = offset;
    return s;
}

static int64_t sim_poll(void *ctx, void *handle, uint32_t now_ms, int64_t *total, bool *done, char **error)
{
    (void)ctx;
    Sim *s = handle;
    if (!s->started) {
        s->started = true;
        s->start_ms = now_ms;
    }
    int64_t got = s->offset + (int64_t)(now_ms - s->start_ms) * s->rate / 1000;
    if (s->fail && got >= s->size / 2) {
        *error = au_strndup("The server closed the connection", 32);
        return s->size / 2;
    }
    if (got >= s->size) {
        got = s->size;
        *done = true;
        *total = s->size;
    } else if (!s->hide_size) {
        *total = s->size;
    }
    return got;
}

static void sim_stop(void *ctx, void *handle)
{
    (void)ctx;
    au_free(handle);
}

const AuTransferSource *au_sim_source(void)
{
    static const AuTransferSource src = {NULL, sim_start, sim_poll, sim_stop};
    return &src;
}

/* ---- List painting ---- */

int au_downloads_hit(const AuDownloads *dl, int y, int scroll)
{
    int i = (y + scroll) / AU_DL_ROW_H;
    return y + scroll >= 0 && i < dl->n ? i : -1;
}

static void row_text(const AuDownload *d, char *out, size_t cap)
{
    char a[32], b[32];
    au_format_size(d->received, a, sizeof a);
    if (d->total >= 0)
        au_format_size(d->total, b, sizeof b);
    switch (d->state) {
    case AU_DL_ACTIVE: {
        char rate[32] = "", left[32] = "";
        if (d->rate) {
            au_format_size(d->rate, rate, sizeof rate);
            long eta = au_download_eta(d);
            if (eta >= 0)
                snprintf(left, sizeof left, ", %ld:%02ld left", eta / 60, eta % 60);
        }
        if (d->total >= 0)
            snprintf(out, cap, "%s of %s%s%s/s%s", a, b, d->rate ? " at " : "", rate, left);
        else
            snprintf(out, cap, "%s%s%s%s", a, d->rate ? " at " : "", rate, d->rate ? "/s" : "");
        if (!d->rate)
            snprintf(out + strlen(out), cap - strlen(out), ", starting");
        break;
    }
    case AU_DL_QUEUED: snprintf(out, cap, "Waiting"); break;
    case AU_DL_PAUSED:
        snprintf(out, cap, "%s, %s of %s", d->interrupted ? "Interrupted" : "Stopped", a, d->total >= 0 ? b : "?");
        break;
    case AU_DL_DONE: snprintf(out, cap, "Done, %s", a); break;
    case AU_DL_FAILED: snprintf(out, cap, "Failed: %s", d->error); break;
    case AU_DL_CANCELLED: snprintf(out, cap, "Cancelled"); break;
    }
}

void au_downloads_paint(const AuDownloads *dl, AuCanvas *c, AuFontSet *fonts, int width, int height, int scroll,
                        int selected, bool active_window)
{
    AuRect all = {0, 0, (float)width, (float)height};
    au_canvas_clip_push(c, all);
    au_fill_rect(c, all, AU_RGB(255, 255, 255));
    AuFontFace *f = fonts ? au_chrome_label_face(fonts) : NULL;
    AuFontFace *small = fonts ? au_fontset_match(fonts, "Geneva, system-ui", 400, false) : NULL;
    char buf[300], name[200];
    for (int i = scroll / AU_DL_ROW_H; i < dl->n; i++) {
        float top = (float)(i * AU_DL_ROW_H - scroll);
        if (top >= (float)height)
            break;
        const AuDownload *d = dl->items[i];
        bool sel = i == selected;
        AuColor ink = AU_RGB(0, 0, 0), dim = AU_RGB(110, 110, 110);
        if (sel) {
            au_fill_rect(c, (AuRect){0, top, (float)width, AU_DL_ROW_H},
                         active_window ? AU_RGB(102, 102, 204) : AU_RGB(204, 204, 204));
            if (active_window)
                ink = AU_RGB(255, 255, 255), dim = AU_RGB(230, 230, 255);
        }
        au_fill_rect(c, (AuRect){0, top + AU_DL_ROW_H - 1, (float)width, 1}, AU_RGB(221, 221, 221));
        /* A document icon. */
        au_fill_rect(c, (AuRect){10, top + 8, 18, 24}, AU_RGB(102, 102, 102));
        au_fill_rect(c, (AuRect){11, top + 9, 16, 22},
                     d->state == AU_DL_DONE ? AU_RGB(255, 255, 255) : AU_RGB(238, 238, 238));
        float tx = 38, tw = (float)width - tx - 10;
        if (f) {
            au_chrome_ellipsize(f, 12, d->filename, tw * 0.6f, name, sizeof name);
            au_draw_text(c, f, 12, tx, top + 14, name, strlen(name), ink);
        }
        bool bar = d->state == AU_DL_ACTIVE || d->state == AU_DL_PAUSED || d->state == AU_DL_QUEUED;
        if (bar) {
            AuRect pb = {tx, top + 18, tw, 8};
            au_fill_rect(c, pb, AU_RGB(102, 102, 102));
            au_fill_rect(c, (AuRect){pb.x + 1, pb.y + 1, pb.w - 2, pb.h - 2}, AU_RGB(238, 238, 238));
            if (d->total > 0) {
                float fr = (float)d->received / (float)d->total;
                if (fr > 1)
                    fr = 1;
                au_fill_rect(c, (AuRect){pb.x + 1, pb.y + 1, (pb.w - 2) * fr, pb.h - 2},
                             d->state == AU_DL_ACTIVE ? AU_RGB(102, 102, 204) : AU_RGB(160, 160, 160));
            } else if (d->state == AU_DL_ACTIVE) {
                /* Unknown size: a moving stripe. */
                float x = (float)((d->received / 4096) % 100) / 100 * (pb.w - 40);
                au_fill_rect(c, (AuRect){pb.x + 1 + x, pb.y + 1, 38, pb.h - 2}, AU_RGB(102, 102, 204));
            }
        }
        if (small) {
            row_text(d, buf, sizeof buf);
            au_chrome_ellipsize(small, 10, buf, tw, name, sizeof name);
            au_draw_text(c, small, 10, tx, top + (bar ? 36 : 30), name, strlen(name), dim);
            if (f) {
                /* The URL beside the name. */
                float nw = au_text_width(f, 12, d->filename, strlen(d->filename));
                if (nw > tw * 0.6f)
                    nw = tw * 0.6f;
                au_chrome_ellipsize(small, 10, d->url, tw - nw - 12, name, sizeof name);
                au_draw_text(c, small, 10, tx + nw + 12, top + 14, name, strlen(name), dim);
            }
        }
    }
    au_canvas_clip_pop(c);
}
