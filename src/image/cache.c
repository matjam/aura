/* The per-document image cache. See cache.h. */
#include "cache.h"

#include <stdlib.h>
#include <string.h>

struct AuImageEntry {
    char *url;
    AuBuf data;          /* compressed bytes */
    bool loading;        /* more bytes may come */
    bool data_dropped;   /* the bytes were dropped: the pixels are kept */
    AuImageState state;
    int iw, ih;
    uint8_t missing;     /* AuImageInfo.missing */
    AuImageFormat format;
    AuImageDecoder *probe; /* header only, fed as bytes arrive */
    /* Pixels. */
    AuSurface view;      /* the stable surface layout holds */
    AuPixel blank;
    AuSurface *pixels;   /* a finished canvas, owned */
    AuImageDecoder *dec; /* a decoder alive: filling in or animating */
    size_t dec_pos;      /* bytes of data the decoder has consumed */
    int tw, th;          /* the target it decodes at */
    bool waiting;        /* an animation frame is shown; the next waits for its time */
    bool animated;
    int loop;
    uint32_t plays;
    uint32_t frame_due;
    uint32_t last_used;  /* epoch */
    int want_w, want_h;  /* the largest display size asked for in this epoch */
    /* Painted at half the decoded size or less in small_epochs epochs in
     * a row, at most small_w x small_h (shrink_when_small). */
    uint8_t small_epochs;
    int small_w, small_h;
    bool changed;
};

struct AuImageCache {
    AuImageEntry **e;
    uint32_t n, cap;
    size_t budget;
    uint32_t epoch;
    uint32_t now;
    bool layout_changed;
    uint32_t decodes, evictions, decode_ms;
    uint32_t (*clock)(void *ctx);
    void *clock_ctx;
    struct AuFontSet *fonts; /* text in SVG images */
    uint8_t keep_format; /* AU_SURF_*: what finished opaque still images are kept in */
};

void au_image_cache_set_fonts(AuImageCache *c, struct AuFontSet *fonts) { c->fonts = fonts; }

void au_image_cache_set_depth(AuImageCache *c, int bits)
{
    c->keep_format = bits == 16 ? AU_SURF_RGB565 : bits == 8 ? AU_SURF_INDEX8 : AU_SURF_ARGB32;
}

AuImageCache *au_image_cache_new(size_t budget)
{
    AuImageCache *c = au_calloc(sizeof *c);
    c->budget = budget ? budget : AU_IMAGE_CACHE_DEFAULT_BUDGET;
    c->epoch = 2;
    return c;
}

static void drop_pixels(AuImageEntry *e)
{
    au_image_decoder_free(e->dec);
    e->dec = NULL;
    au_surface_free(e->pixels);
    e->pixels = NULL;
    e->waiting = false;
    e->view.pixels = &e->blank;
    e->view.width = e->view.height = e->view.stride = 1;
    e->view.opaque = false;
}

static void free_entry(AuImageEntry *e)
{
    drop_pixels(e);
    au_image_decoder_free(e->probe);
    au_buf_free(&e->data);
    au_free(e->url);
    au_free(e);
}

void au_image_cache_free(AuImageCache *c)
{
    if (!c)
        return;
    for (uint32_t i = 0; i < c->n; i++)
        free_entry(c->e[i]);
    au_free(c->e);
    au_free(c);
}

void au_image_cache_set_budget(AuImageCache *c, size_t budget)
{
    c->budget = budget ? budget : AU_IMAGE_CACHE_DEFAULT_BUDGET;
    au_image_cache_trim(c);
}

void au_image_cache_set_clock(AuImageCache *c, uint32_t (*now_ms)(void *ctx), void *ctx)
{
    c->clock = now_ms;
    c->clock_ctx = ctx;
}

static uint32_t clock_now(AuImageCache *c) { return c->clock ? c->clock(c->clock_ctx) : 0; }

AuImageEntry *au_image_cache_find(AuImageCache *c, const char *url)
{
    for (uint32_t i = 0; i < c->n; i++)
        if (!strcmp(c->e[i]->url, url))
            return c->e[i];
    return NULL;
}

AuImageEntry *au_image_cache_add(AuImageCache *c, const char *url)
{
    AuImageEntry *e = au_image_cache_find(c, url);
    if (e)
        return e;
    e = au_calloc(sizeof *e);
    e->url = au_strndup(url, strlen(url));
    au_buf_init(&e->data);
    e->loading = true;
    e->state = AU_IMAGE_PENDING;
    e->view.pixels = &e->blank;
    e->view.width = e->view.height = e->view.stride = 1;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->e = au_realloc(c->e, c->cap * sizeof *c->e);
    }
    c->e[c->n++] = e;
    return e;
}

const char *au_image_entry_url(const AuImageEntry *e) { return e->url; }
AuImageState au_image_entry_state(const AuImageEntry *e) { return e->state; }
const AuSurface *au_image_entry_surface(AuImageEntry *e) { return &e->view; }
bool au_image_entry_animated(const AuImageEntry *e) { return e->animated; }

bool au_image_entry_size(const AuImageEntry *e, int *w, int *h)
{
    if (e->state != AU_IMAGE_SIZED && e->state != AU_IMAGE_READY)
        return false;
    *w = e->iw;
    *h = e->ih;
    return true;
}

unsigned au_image_entry_missing(const AuImageEntry *e)
{
    return e->state == AU_IMAGE_SIZED || e->state == AU_IMAGE_READY ? e->missing : 0;
}

bool au_image_entry_take_changed(AuImageEntry *e)
{
    bool ch = e->changed;
    e->changed = false;
    return ch;
}

bool au_image_cache_take_layout_changed(AuImageCache *c)
{
    bool ch = c->layout_changed;
    c->layout_changed = false;
    return ch;
}

static size_t image_cap(const AuImageCache *c)
{
    size_t cap = c->budget / 2;
    if (cap < (size_t)1024 * 1024)
        cap = (size_t)1024 * 1024;
    return cap < AU_IMAGE_CACHE_MAX_IMAGE ? cap : AU_IMAGE_CACHE_MAX_IMAGE;
}

/* Points the stable surface at the current pixels. */
static void sync_view(AuImageEntry *e)
{
    const AuSurface *s = e->pixels ? e->pixels : e->dec ? au_image_canvas(e->dec) : NULL;
    if (!s || s->width <= 0 || s->height <= 0) {
        e->view.pixels = &e->blank;
        e->view.width = e->view.height = e->view.stride = 1;
        e->view.opaque = false;
        return;
    }
    e->view.pixels = s->pixels;
    e->view.width = s->width;
    e->view.height = s->height;
    e->view.stride = s->stride;
    e->view.opaque = s->opaque;
    e->view.format = s->format;
}

static void set_sized(AuImageCache *c, AuImageEntry *e, const AuImageInfo *info)
{
    int w = info->width, h = info->height;
    if (e->state == AU_IMAGE_PENDING || e->iw != w || e->ih != h || e->missing != info->missing) {
        c->layout_changed = true;
        e->changed = true; /* repaint where it shows */
    }
    e->iw = w;
    e->ih = h;
    e->missing = info->missing;
    e->format = info->format;
    if (e->state == AU_IMAGE_PENDING)
        e->state = AU_IMAGE_SIZED;
}

static void set_failed(AuImageCache *c, AuImageEntry *e)
{
    if (e->state != AU_IMAGE_FAILED) {
        c->layout_changed = true;
        e->changed = true;
    }
    e->state = AU_IMAGE_FAILED;
    drop_pixels(e);
    sync_view(e);
    /* A failed image is never decoded again: its bytes are not needed. */
    au_buf_free(&e->data);
    au_buf_init(&e->data);
    e->dec_pos = 0;
}

/* The decoder is finished with: its canvas becomes the entry's pixels. */
static void finalize(AuImageCache *c, AuImageEntry *e)
{
    if (!e->dec)
        return;
    const AuImageInfo *info = au_image_info(e->dec);
    int denom = info->scale_denom;
    AuSurface *s = au_image_take_canvas(e->dec);
    au_image_decoder_free(e->dec);
    e->dec = NULL;
    e->waiting = false;
    au_surface_free(e->pixels);
    /* A finished opaque still image is kept in the cache's format: 16 or 8
     * bits dithered (au_image_cache_set_depth). Animations and images with
     * transparency stay ARGB32. */
    if (s && c->keep_format != AU_SURF_ARGB32 && s->opaque && !e->animated && e->format != AU_IMAGE_FORMAT_SVG) {
        AuSurface *k = au_surface_convert(s, c->keep_format);
        if (k) {
            au_surface_free(s);
            s = k;
        }
    }
    e->pixels = s;
    sync_view(e);
    /* Keep whichever is smaller: the bytes or full-size still pixels. An
     * SVG keeps its bytes: it is drawn again at each new size. */
    if (s && !e->animated && !e->loading && denom == 1 && e->format != AU_IMAGE_FORMAT_SVG &&
        e->data.len > au_surface_bytes(s)) {
        au_buf_free(&e->data);
        au_buf_init(&e->data);
        e->data_dropped = true;
    }
}

static bool may_animate(const AuImageEntry *e, const AuImageInfo *info)
{
    return info->animated || e->format == AU_IMAGE_FORMAT_GIF;
}

/* Runs the decoder over the bytes it has not seen. Decodes at most one
 * frame. */
static void pump(AuImageCache *c, AuImageEntry *e)
{
    if (!e->dec || e->waiting)
        return;
    uint32_t t0 = clock_now(c);
    const uint8_t *p = (const uint8_t *)e->data.data + e->dec_pos;
    size_t n = e->data.len - e->dec_pos;
    AuImageStatus st = au_image_decode(e->dec, &p, &n, !e->loading);
    e->dec_pos = e->data.len - n;
    const AuImageInfo *info = au_image_info(e->dec);
    c->decode_ms += clock_now(c) - t0;
    if (info->ready) {
        set_sized(c, e, info);
        if (au_image_canvas(e->dec))
            e->state = AU_IMAGE_READY;
    }
    AuIRect r;
    if (au_image_take_dirty(e->dec, &r))
        e->changed = true;
    switch (st) {
    case AU_IMAGE_MORE:
        break;
    case AU_IMAGE_FRAME:
        e->changed = true;
        if (info->frame_count == 1 && !may_animate(e, info)) {
            finalize(c, e);
            break;
        }
        if (info->frame_count > 1) {
            e->animated = true;
            e->loop = info->loop_count;
        }
        e->waiting = true;
        e->frame_due = c->now + au_image_frame_delay(info->frame_delay_ms);
        break;
    case AU_IMAGE_DONE:
        if (info->frame_count > 1 && (e->loop == 0 || e->plays + 1 < (uint32_t)e->loop)) {
            /* Loop: start again from the first frame. */
            e->plays++;
            AuImageOptions o = {0};
            o.target_w = e->tw;
            o.target_h = e->th;
            o.max_bytes = image_cap(c);
            o.fonts = c->fonts;
            au_image_decoder_free(e->dec);
            e->dec = au_image_decoder_new(&o);
            e->dec_pos = 0;
            sync_view(e);
            pump(c, e);
            return;
        }
        if (info->frame_count > 1)
            e->animated = true; /* played out: the last frame stays */
        finalize(c, e);
        break;
    case AU_IMAGE_ERROR:
        if (info->partial || info->frame_count > 0)
            finalize(c, e); /* keep what decoded */
        else if (!e->loading || !info->ready)
            set_failed(c, e);
        else
            drop_pixels(e);
        break;
    }
    sync_view(e);
}

static void start_decode(AuImageCache *c, AuImageEntry *e, int tw, int th)
{
    if (e->data_dropped)
        return;
    AuImageOptions o = {0};
    o.target_w = tw;
    o.target_h = th;
    o.max_bytes = image_cap(c);
    o.fonts = c->fonts;
    au_image_decoder_free(e->dec);
    e->dec = au_image_decoder_new(&o);
    e->dec_pos = 0;
    e->waiting = false;
    e->plays = 0;
    e->tw = tw;
    e->th = th;
    c->decodes++;
    pump(c, e);
}

void au_image_entry_data(AuImageCache *c, AuImageEntry *e, const void *data, size_t len)
{
    if (!e->loading || !len || e->state == AU_IMAGE_FAILED)
        return;
    au_buf_append(&e->data, data, len);
    if (e->state == AU_IMAGE_PENDING && !e->dec) {
        /* Read the header as it arrives. */
        if (!e->probe) {
            AuImageOptions o = {0};
            o.header_only = true;
            e->probe = au_image_decoder_new(&o);
        }
        const uint8_t *p = data;
        size_t n = len;
        AuImageStatus st = au_image_decode(e->probe, &p, &n, false);
        const AuImageInfo *info = au_image_info(e->probe);
        if (info->ready)
            set_sized(c, e, info);
        if (st == AU_IMAGE_ERROR)
            set_failed(c, e);
        if (st != AU_IMAGE_MORE) {
            au_image_decoder_free(e->probe);
            e->probe = NULL;
        }
    }
    /* An image in use fills in as its bytes arrive. */
    if (e->dec && !e->waiting)
        pump(c, e);
}

void au_image_entry_done(AuImageCache *c, AuImageEntry *e, bool ok)
{
    if (!e->loading)
        return;
    e->loading = false;
    if (e->probe) {
        au_image_decoder_free(e->probe);
        e->probe = NULL;
    }
    if (!ok && e->state != AU_IMAGE_READY) {
        set_failed(c, e);
        return;
    }
    if (e->state == AU_IMAGE_PENDING && !e->dec) {
        /* No header in all the bytes: one last try with everything. */
        AuImageInfo info;
        if (au_image_probe(e->data.data, e->data.len, &info))
            set_sized(c, e, &info);
        else
            set_failed(c, e);
    }
    if (e->dec && !e->waiting)
        pump(c, e);
    if (e->state == AU_IMAGE_PENDING)
        set_failed(c, e);
}

static void start_decode(AuImageCache *c, AuImageEntry *e, int tw, int th);

/* An image decoded at its full size before layout asked for a size (a
 * prefetch below the view) and painted much smaller was box-filtered
 * down on every paint and kept at full size: Reddit's feed images were a
 * third of the paint time on the host. Once it has been painted at half
 * its decoded size or less SHRINK_EPOCHS times in a row, it is decoded
 * again at the largest of those sizes (a power-of-two reduction, at
 * least that size). A paint of one strip that leaves out a larger use
 * of the same image delays this; one that shows it larger starts over. */
#define SHRINK_EPOCHS 8
static void shrink_when_small(AuImageCache *c, AuImageEntry *e)
{    if (e->last_used != c->epoch || !e->pixels || e->dec || e->loading || e->data_dropped || e->animated ||
        e->format == AU_IMAGE_FORMAT_SVG || e->want_w <= 0 || e->want_h <= 0)
        return;
    const AuSurface *s = e->pixels;
    if (s->width < 2 * e->want_w || s->height < 2 * e->want_h) {
        e->small_epochs = 0;
        return;
    }
    if (!e->small_epochs || e->want_w > e->small_w)
        e->small_w = e->want_w;
    if (!e->small_epochs || e->want_h > e->small_h)
        e->small_h = e->want_h;
    if (++e->small_epochs < SHRINK_EPOCHS)
        return;
    e->small_epochs = 0;
    if (s->width < 2 * e->small_w || s->height < 2 * e->small_h)
        return;
    start_decode(c, e, e->small_w, e->small_h);
}

void au_image_cache_frame_begin(AuImageCache *c)
{
    for (uint32_t i = 0; i < c->n; i++)
        shrink_when_small(c, c->e[i]);
    c->epoch++;
    for (uint32_t i = 0; i < c->n; i++)
        c->e[i]->want_w = c->e[i]->want_h = 0;
}

/* Whether a display size asks for more than a decode target did. 0 in
 * either means no limit (the natural size). */
static bool wants_more(int want, int target)
{
    if (!target)
        return false;
    return !want || want > target;
}

void au_image_entry_use(AuImageCache *c, AuImageEntry *e, int dw, int dh)
{
    bool resumed = e->last_used + 1 < c->epoch;
    e->last_used = c->epoch;
    if (dw < 0)
        dw = 0;
    if (dh < 0)
        dh = 0;
    if (dw > e->want_w)
        e->want_w = dw;
    if (dh > e->want_h)
        e->want_h = dh;
    if (e->state == AU_IMAGE_FAILED)
        return;
    if (e->waiting && resumed)
        e->frame_due = c->now; /* an animation coming back into view */
    bool have = e->pixels || e->dec;
    if (!have) {
        if (e->state == AU_IMAGE_SIZED || (e->state == AU_IMAGE_PENDING && e->loading && e->data.len)) {
            start_decode(c, e, e->want_w, e->want_h);
            au_image_cache_trim(c);
        }
        return;
    }
    const AuSurface *s = &e->view;
    if (e->format == AU_IMAGE_FORMAT_SVG) {
        /* A vector image is drawn again when it is shown at another size
         * than it was drawn at: larger (to stay sharp) or of another
         * shape (the document fits its viewBox into the box). */
        if (!e->loading && e->pixels && e->want_w > 0 && e->want_h > 0 &&
            (e->want_w > s->width + 1 || e->want_h > s->height + 1 ||
             abs(e->want_w * s->height - e->want_h * s->width) > (s->width + s->height) * 2)) {
            start_decode(c, e, e->want_w, e->want_h);
            au_image_cache_trim(c);
        }
        return;
    }
    /* Shown larger than decoded (and decoded reduced): decode again, but
     * only when asked for more than the last decode's target. An image
     * the size cap reduced comes out reduced at any target, and decoding
     * it again on each paint made every scroll step a full decode. */
    bool reduced = s->width < e->iw || s->height < e->ih;
    if (reduced && !e->loading && !e->data_dropped && ((dw && s->width < dw) || (dh && s->height < dh) || (!dw && !dh)) &&
        (wants_more(dw, e->tw) || wants_more(dh, e->th))) {
        start_decode(c, e, e->want_w, e->want_h);
        au_image_cache_trim(c);
    }
}

static size_t entry_bytes(const AuImageEntry *e)
{
    size_t b = 0;
    if (e->pixels)
        b += au_surface_bytes(e->pixels);
    if (e->dec)
        b += au_image_info(e->dec)->mem_bytes;
    return b;
}

void au_image_cache_trim(AuImageCache *c)
{
    size_t total = 0;
    for (uint32_t i = 0; i < c->n; i++)
        total += entry_bytes(c->e[i]);
    while (total > c->budget) {
        AuImageEntry *victim = NULL;
        for (uint32_t i = 0; i < c->n; i++) {
            AuImageEntry *e = c->e[i];
            if (e->last_used >= c->epoch || e->data_dropped || e->loading || !(e->pixels || e->dec))
                continue;
            if (!victim || e->last_used < victim->last_used)
                victim = e;
        }
        if (!victim)
            break;
        total -= entry_bytes(victim);
        drop_pixels(victim);
        victim->state = AU_IMAGE_SIZED;
        /* Not marked changed: what is on screen stays right, and the
         * image decodes again when it is painted next (marking it would
         * repaint it at once and decode it again, every frame). */
        c->evictions++;
    }
}

uint32_t au_image_cache_tick(AuImageCache *c, uint32_t now_ms)
{
    c->now = now_ms;
    uint32_t next = UINT32_MAX;
    for (uint32_t i = 0; i < c->n; i++) {
        AuImageEntry *e = c->e[i];
        if (!e->dec || !e->waiting)
            continue;
        if (e->last_used != c->epoch)
            continue; /* not used in the last paint: paused */
        if ((int32_t)(now_ms - e->frame_due) >= 0) {
            e->waiting = false;
            pump(c, e);
            if (e->dec && e->waiting && (int32_t)(e->frame_due - now_ms) < 0)
                e->frame_due = now_ms; /* fell behind: do not try to catch up */
        }
        if (e->dec && e->waiting) {
            uint32_t d = e->frame_due - now_ms;
            if ((int32_t)d < 0)
                d = 0;
            if (d < next)
                next = d;
        }
    }
    return next;
}

uint32_t au_image_cache_count(const AuImageCache *c) { return c->n; }
AuImageEntry *au_image_cache_entry(AuImageCache *c, uint32_t i) { return i < c->n ? c->e[i] : NULL; }

void au_image_cache_stats(const AuImageCache *c, AuImageCacheStats *s)
{
    memset(s, 0, sizeof *s);
    s->entries = c->n;
    s->budget = c->budget;
    s->decodes = c->decodes;
    s->evictions = c->evictions;
    s->decode_ms = c->decode_ms;
    for (uint32_t i = 0; i < c->n; i++) {
        const AuImageEntry *e = c->e[i];
        if (e->pixels) {
            s->decoded++;
            s->decoded_bytes += au_surface_bytes(e->pixels);
        }
        if (e->dec) {
            const AuSurface *cv = au_image_canvas(e->dec);
            size_t cb = cv ? (size_t)cv->width * cv->height * 4 : 0;
            s->decoded += cv != NULL;
            s->decoded_bytes += cb;
            s->decoder_bytes += au_image_info(e->dec)->mem_bytes - cb;
            if (e->waiting && e->last_used == c->epoch)
                s->animating++; /* in use: paused animations do not count */
        }
        s->compressed_bytes += e->data.len;
    }
}
