#include "favicon.h"

#include "image/image.h"
#include "url.h"

#include <string.h>

typedef struct Icon {
    char *host;
    AuSurface *pix; /* 16 x 16, or NULL: the host has no icon (not saved) */
    AuTime when;    /* fetched */
} Icon;

typedef struct IconFetch {
    AuFavicons *owner;
    uint32_t id; /* 0: free */
    AuPageFetcher fetcher;
    char *host;
    AuBuf bytes;
    int status;
    bool too_large;
    AuTime when;
} IconFetch;

struct AuFavicons {
    Icon *icons;
    size_t n, cap;
    IconFetch fetches[AU_FAVICON_FETCHES];
    bool dirty;
    uint32_t serial; /* bumped when an icon arrives or the store is cleared */
};

AuFavicons *au_favicons_new(void)
{
    return au_calloc(sizeof(AuFavicons));
}

static void icon_free(Icon *ic)
{
    au_free(ic->host);
    if (ic->pix)
        au_surface_free(ic->pix);
}

void au_favicons_clear(AuFavicons *f)
{
    for (size_t i = 0; i < f->n; i++)
        icon_free(&f->icons[i]);
    if (f->n)
        f->dirty = true;
    f->n = 0;
    f->serial++;
}

void au_favicons_cancel(AuFavicons *f)
{
    for (int i = 0; i < AU_FAVICON_FETCHES; i++) {
        IconFetch *q = &f->fetches[i];
        if (!q->id)
            continue;
        uint32_t id = q->id;
        q->id = 0;
        if (q->fetcher.cancel)
            q->fetcher.cancel(q->fetcher.ctx, id);
        au_free(q->host);
        au_buf_free(&q->bytes);
    }
}

void au_favicons_free(AuFavicons *f)
{
    if (!f)
        return;
    au_favicons_cancel(f);
    au_favicons_clear(f);
    au_free(f->icons);
    au_free(f);
}

size_t au_favicons_count(const AuFavicons *f)
{
    size_t n = 0;
    for (size_t i = 0; i < f->n; i++)
        if (f->icons[i].pix)
            n++;
    return n;
}

/* The lower-case host of an http or https URL; false for anything else. */
static bool site_host(const char *url, char *host, size_t cap)
{
    if (!url || (!au_url_scheme_is(url, "http") && !au_url_scheme_is(url, "https")))
        return false;
    au_url_host(url, host, cap);
    if (!host[0])
        return false;
    for (char *p = host; *p; p++)
        *p = (char)au_lower((unsigned char)*p);
    return true;
}

static Icon *find(AuFavicons *f, const char *host)
{
    for (size_t i = 0; i < f->n; i++)
        if (!strcmp(f->icons[i].host, host))
            return &f->icons[i];
    return NULL;
}

const AuSurface *au_favicons_for_url(AuFavicons *f, const char *url)
{
    char host[256];
    if (!f || !site_host(url, host, sizeof host))
        return NULL;
    Icon *ic = find(f, host);
    return ic ? ic->pix : NULL;
}

const AuSurface *au_favicons_for_host(AuFavicons *f, const char *host)
{
    Icon *ic = f && host ? find(f, host) : NULL;
    return ic ? ic->pix : NULL;
}

bool au_favicons_host(const char *url, char *host, size_t cap)
{
    return site_host(url, host, cap);
}

/* The entry for host, made if needed; the oldest goes when full. */
static Icon *entry(AuFavicons *f, const char *host)
{
    Icon *ic = find(f, host);
    if (ic)
        return ic;
    if (f->n >= AU_FAVICON_MAX) {
        size_t old = 0;
        for (size_t i = 1; i < f->n; i++)
            if (f->icons[i].when < f->icons[old].when)
                old = i;
        icon_free(&f->icons[old]);
        f->icons[old] = f->icons[--f->n];
    }
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->icons = au_realloc(f->icons, f->cap * sizeof *f->icons);
    }
    ic = &f->icons[f->n++];
    ic->host = au_strndup(host, strlen(host));
    ic->pix = NULL;
    ic->when = 0;
    return ic;
}

/* An icon's surface: kept as long as the store, so in the long-lived zone
 * although src/paint makes it (docs/memory.md, "Zones"). */
static AuSurface *icon_surface(void)
{
    int h = au_heap_zone_hold(AU_HEAP_ZONE_LONG);
    AuSurface *s = au_surface_new(AU_FAVICON_SIZE, AU_FAVICON_SIZE);
    au_heap_zone_unhold(h);
    return s;
}

static AuSurface *copy16(const AuSurface *s)
{
    if (!s || s->width != AU_FAVICON_SIZE || s->height != AU_FAVICON_SIZE)
        return NULL;
    AuSurface *d = icon_surface();
    for (int y = 0; y < AU_FAVICON_SIZE; y++)
        memcpy(d->pixels + (size_t)y * (size_t)d->stride, s->pixels + (size_t)y * (size_t)s->stride,
               AU_FAVICON_SIZE * sizeof(AuPixel));
    d->opaque = s->opaque;
    return d;
}

void au_favicons_put(AuFavicons *f, const char *host, const AuSurface *icon, AuTime when)
{
    AuSurface *pix = copy16(icon);
    Icon *ic = entry(f, host);
    if (ic->pix) {
        au_surface_free(ic->pix);
        f->dirty = true;
    }
    ic->pix = pix; /* NULL records that the host has no icon */
    ic->when = when;
    if (pix)
        f->dirty = true;
    f->serial++;
}

/* ---- Fetching ---- */

static IconFetch *fetch_of(void *ctx, uint32_t id)
{
    IconFetch *q = ctx;
    return q->id == id && id ? q : NULL;
}

static void s_response(void *ctx, uint32_t id, const AuFetchResponse *r)
{
    IconFetch *q = fetch_of(ctx, id);
    if (q)
        q->status = r->status;
}

static void s_data(void *ctx, uint32_t id, const void *bytes, size_t len)
{
    IconFetch *q = fetch_of(ctx, id);
    if (!q || q->too_large)
        return;
    if (q->bytes.len + len > AU_FAVICON_MAX_BYTES) {
        q->too_large = true;
        au_buf_free(&q->bytes);
        return;
    }
    au_buf_append(&q->bytes, bytes, len);
}

static void s_done(void *ctx, uint32_t id, bool ok, const AuFetchInfo *info)
{
    IconFetch *q = fetch_of(ctx, id);
    if (!q)
        return;
    int status = info && info->status ? info->status : q->status;
    AuSurface *pix = NULL;
    /* status 0: a fetcher that reports none (data read from a file). */
    if (ok && !q->too_large && q->bytes.len && (status == 0 || status == 200))
        pix = au_image_decode_favicon(q->bytes.data, q->bytes.len, AU_FAVICON_SIZE);
    AuFavicons *f = q->owner;
    Icon *old = find(f, q->host);
    if (pix || !old || !old->pix)
        au_favicons_put(f, q->host, pix, q->when); /* a failed refresh keeps the icon it had */
    else
        old->when = q->when;
    if (pix)
        au_surface_free(pix);
    q->id = 0;
    au_free(q->host);
    q->host = NULL;
    au_buf_free(&q->bytes);
}

int au_favicons_pending(const AuFavicons *f)
{
    int n = 0;
    for (int i = 0; i < AU_FAVICON_FETCHES; i++)
        if (f->fetches[i].id)
            n++;
    return n;
}

void au_favicons_request(AuFavicons *f, const AuPageFetcher *fetcher, const char *page_url, const char *icon_url,
                         AuTime now)
{
    char host[256], ihost[256];
    if (!f || !fetcher || !fetcher->start || !icon_url || !site_host(page_url, host, sizeof host) ||
        !site_host(icon_url, ihost, sizeof ihost))
        return;
    Icon *ic = find(f, host);
    if (ic && (!ic->pix || now - ic->when < AU_FAVICON_FRESH_S))
        return;
    IconFetch *q = NULL;
    for (int i = 0; i < AU_FAVICON_FETCHES; i++) {
        if (f->fetches[i].id && !strcmp(f->fetches[i].host, host))
            return;
        if (!f->fetches[i].id && !q)
            q = &f->fetches[i];
    }
    if (!q)
        return; /* busy: the next load of the site asks again */
    memset(q, 0, sizeof *q);
    q->owner = f;
    q->fetcher = *fetcher;
    q->host = au_strndup(host, strlen(host));
    q->when = now;
    au_buf_init(&q->bytes);
    AuFetchRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = icon_url;
    rq.kind = AU_FETCH_IMAGE;
    rq.document_url = page_url;
    rq.top_level_url = page_url;
    rq.cache = AU_FETCH_CACHE_NORMAL;
    rq.omit_credentials = strcmp(host, ihost) != 0; /* no cookies to another host */
    AuFetchSink sink = {q, s_response, s_data, s_done, NULL};
    /* start never calls the sink, so the id is set before any callback. */
    q->id = fetcher->start(fetcher->ctx, &rq, &sink);
    if (!q->id) {
        au_free(q->host);
        q->host = NULL;
        au_buf_free(&q->bytes);
        /* Refused before a request was made (a blocked or malformed URL). */
        au_favicons_put(f, host, NULL, now);
    }
}

uint32_t au_favicons_idle(AuFavicons *f)
{
    if (!f)
        return 0;
    for (int i = 0; i < AU_FAVICON_FETCHES; i++) {
        IconFetch *q = &f->fetches[i];
        if (!q->id || !q->fetcher.poll)
            continue;
        /* One poll per fetcher. */
        bool polled = false;
        for (int k = 0; k < i; k++)
            if (f->fetches[k].id && f->fetches[k].fetcher.poll == q->fetcher.poll &&
                f->fetches[k].fetcher.ctx == q->fetcher.ctx)
                polled = true;
        if (!polled)
            q->fetcher.poll(q->fetcher.ctx);
    }
    return f->serial;
}

/* ---- Persistence ----
 * "AUFI" 1, a 16-bit count, then per icon: host length (1 byte), host,
 * the fetch time (8 bytes), and 256 pixels as premultiplied A, R, G, B
 * bytes. All integers big-endian. */

bool au_favicons_dirty(const AuFavicons *f)
{
    return f->dirty;
}

void au_favicons_serialize(AuFavicons *f, AuBuf *out)
{
    size_t n = au_favicons_count(f);
    au_buf_append(out, "AUFI\1", 5);
    au_buf_putc(out, (char)(n >> 8));
    au_buf_putc(out, (char)n);
    for (size_t i = 0; i < f->n; i++) {
        const Icon *ic = &f->icons[i];
        size_t hl = strlen(ic->host);
        if (!ic->pix || hl > 255)
            continue;
        au_buf_putc(out, (char)hl);
        au_buf_append(out, ic->host, hl);
        uint64_t t = (uint64_t)ic->when;
        for (int b = 7; b >= 0; b--)
            au_buf_putc(out, (char)(t >> (8 * b)));
        for (int y = 0; y < AU_FAVICON_SIZE; y++) {
            const AuPixel *row = ic->pix->pixels + (size_t)y * (size_t)ic->pix->stride;
            for (int x = 0; x < AU_FAVICON_SIZE; x++) {
                uint32_t p = row[x];
                au_buf_putc(out, (char)(p >> 24));
                au_buf_putc(out, (char)(p >> 16));
                au_buf_putc(out, (char)(p >> 8));
                au_buf_putc(out, (char)p);
            }
        }
    }
    f->dirty = false;
}

bool au_favicons_parse(AuFavicons *f, const void *data, size_t len)
{
    const uint8_t *p = data, *end = p + len;
    au_favicons_clear(f);
    f->dirty = false;
    if (len < 7 || memcmp(p, "AUFI\1", 5) != 0)
        return false;
    size_t n = ((size_t)p[5] << 8) | p[6];
    p += 7;
    for (size_t i = 0; i < n; i++) {
        if (end - p < 1)
            goto bad;
        size_t hl = *p++;
        if (!hl || (size_t)(end - p) < hl + 8 + AU_FAVICON_SIZE * AU_FAVICON_SIZE * 4)
            goto bad;
        char host[256];
        memcpy(host, p, hl);
        host[hl] = 0;
        p += hl;
        if (strlen(host) != hl)
            goto bad; /* a NUL inside the name */
        uint64_t t = 0;
        for (int b = 0; b < 8; b++)
            t = (t << 8) | *p++;
        AuSurface *s = icon_surface();
        bool opaque = true;
        for (int y = 0; y < AU_FAVICON_SIZE; y++) {
            AuPixel *row = s->pixels + (size_t)y * (size_t)s->stride;
            for (int x = 0; x < AU_FAVICON_SIZE; x++, p += 4) {
                uint32_t a = p[0], r = p[1], g = p[2], b = p[3];
                /* Premultiplied: no channel above alpha. */
                if (r > a)
                    r = a;
                if (g > a)
                    g = a;
                if (b > a)
                    b = a;
                if (a != 255)
                    opaque = false;
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        s->opaque = opaque;
        if (f->n < AU_FAVICON_MAX && !find(f, host)) {
            Icon *ic = entry(f, host);
            ic->pix = s;
            ic->when = (AuTime)t;
        } else {
            au_surface_free(s);
        }
    }
    return true;
bad:
    au_favicons_clear(f);
    f->dirty = false;
    return false;
}
