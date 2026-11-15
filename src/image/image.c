/* The generic decoder: format sniffing, limits, the canvas and the row
 * helpers every format decoder writes through (image_int.h). */
#include "image_int.h"

#include <stdio.h>

/* ---- Sniffing ---- */

AuImageFormat au_image_sniff(const void *data, size_t len)
{
    const uint8_t *p = data;
    if (len >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
        return AU_IMAGE_FORMAT_PNG;
    if (len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return AU_IMAGE_FORMAT_JPEG;
    if (len >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6)))
        return AU_IMAGE_FORMAT_GIF;
    if (len >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4))
        return AU_IMAGE_FORMAT_WEBP;
    /* BMP: "BM", then a pixel data offset that lands after the 14-byte
     * file header and a known info header size. */
    if (len >= 18 && p[0] == 'B' && p[1] == 'M') {
        uint32_t hs = au_img_le32(p + 14);
        if (hs == 12 || hs == 16 || hs == 40 || hs == 52 || hs == 56 || hs == 64 || hs == 108 || hs == 124)
            return AU_IMAGE_FORMAT_BMP;
    }
    /* ICO: reserved 0, type 1, 1..255 entries, entry reserved byte 0. */
    if (len >= 16 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0 && au_img_le16(p + 4) > 0 &&
        au_img_le16(p + 4) <= 255 && p[9] == 0)
        return AU_IMAGE_FORMAT_ICO;
    return AU_IMAGE_FORMAT_UNKNOWN;
}

const char *au_image_format_name(AuImageFormat f)
{
    switch (f) {
    case AU_IMAGE_FORMAT_PNG: return "PNG";
    case AU_IMAGE_FORMAT_JPEG: return "JPEG";
    case AU_IMAGE_FORMAT_GIF: return "GIF";
    case AU_IMAGE_FORMAT_WEBP: return "WebP";
    case AU_IMAGE_FORMAT_BMP: return "BMP";
    case AU_IMAGE_FORMAT_ICO: return "ICO";
    default: return "unknown";
    }
}

static const AuImgCodec *codec_for(AuImageFormat f)
{
    switch (f) {
    case AU_IMAGE_FORMAT_PNG: return &au_img_png_codec;
    case AU_IMAGE_FORMAT_JPEG: return &au_img_jpeg_codec;
    case AU_IMAGE_FORMAT_GIF: return &au_img_gif_codec;
    case AU_IMAGE_FORMAT_WEBP: return &au_img_webp_codec;
    case AU_IMAGE_FORMAT_BMP: return &au_img_bmp_codec;
    case AU_IMAGE_FORMAT_ICO: return &au_img_ico_codec;
    default: return NULL;
    }
}

/* ---- Decoder ---- */

static void update_mem(AuImageDecoder *d)
{
    size_t m = sizeof *d + d->codec_bytes;
    if (d->canvas)
        m += (size_t)d->canvas->width * (size_t)d->canvas->height * 4 + sizeof *d->canvas;
    if (d->acc)
        m += (size_t)(d->cw / d->red + 1) * 16;
    d->info.mem_bytes = m;
    if (m > d->info.peak_bytes)
        d->info.peak_bytes = m;
}

AuImageDecoder *au_image_decoder_new(const AuImageOptions *opt)
{
    AuImageDecoder *d = au_calloc(sizeof *d);
    if (opt)
        d->opt = *opt;
    if (!d->opt.max_bytes)
        d->opt.max_bytes = AU_IMAGE_DEFAULT_MAX_BYTES;
    if (d->opt.max_bytes < 64)
        d->opt.max_bytes = 64;
    if (!d->opt.max_source_pixels)
        d->opt.max_source_pixels = AU_IMAGE_DEFAULT_MAX_SOURCE_PIXELS;
    if (d->opt.target_w < 0)
        d->opt.target_w = 0;
    if (d->opt.target_h < 0)
        d->opt.target_h = 0;
    d->info.scale_denom = 1;
    d->info.orientation = 1;
    d->red = 1;
    d->orient = 1;
    update_mem(d);
    return d;
}

void au_image_decoder_free(AuImageDecoder *d)
{
    if (!d)
        return;
    if (d->st && d->codec)
        d->codec->destroy(d->st);
    au_surface_free(d->canvas);
    au_free(d->acc);
    au_free(d);
}

const AuImageInfo *au_image_info(const AuImageDecoder *d) { return &d->info; }
const char *au_image_error(const AuImageDecoder *d) { return d->err; }
const AuSurface *au_image_canvas(const AuImageDecoder *d) { return d->canvas; }

AuSurface *au_image_take_canvas(AuImageDecoder *d)
{
    AuSurface *s = d->canvas;
    d->canvas = NULL;
    d->stopped = true;
    update_mem(d);
    return s;
}

bool au_image_take_dirty(AuImageDecoder *d, AuIRect *r)
{
    if (!d->any_dirty)
        return false;
    *r = d->dirty;
    d->any_dirty = false;
    return true;
}

AuImageStatus au_img_fail(AuImageDecoder *d, const char *msg)
{
    if (!d->failed) {
        d->failed = true;
        snprintf(d->err, sizeof d->err, "%s", msg);
    }
    return AU_IMAGE_ERROR;
}

static AuImageStatus run_codec(AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    AuImageStatus st = d->codec->run(d->st, d, in, len, final);
    if (st == AU_IMAGE_ERROR && !d->failed)
        au_img_fail(d, "corrupt image");
    if (d->failed)
        st = AU_IMAGE_ERROR;
    if (st == AU_IMAGE_MORE && final)
        st = au_img_fail(d, "truncated");
    if (st == AU_IMAGE_DONE)
        d->done = true;
    if (st == AU_IMAGE_ERROR && d->info.frame_count == 0 && d->canvas && d->next_row > 0)
        d->info.partial = true;
    if (st == AU_IMAGE_ERROR && d->info.frame_count > 0)
        d->info.partial = true; /* frames before the error are good */
    return st;
}

AuImageStatus au_image_decode(AuImageDecoder *d, const uint8_t **in, size_t *in_len, bool final)
{
    if (d->failed)
        return AU_IMAGE_ERROR;
    if (d->done || d->stopped)
        return AU_IMAGE_DONE;
    if (d->opt.first_frame_only && d->info.frame_count > 0) {
        d->done = true;
        return AU_IMAGE_DONE;
    }
    AuImageStatus st;
    if (!d->codec) {
        /* Sniff from the first 16 bytes (fewer when that is all there is). */
        while (*in_len && d->nhead < sizeof d->head) {
            d->head[d->nhead++] = **in;
            (*in)++;
            (*in_len)--;
        }
        if (d->nhead < sizeof d->head && !final)
            return AU_IMAGE_MORE;
        AuImageFormat f = au_image_sniff(d->head, d->nhead);
        d->codec = codec_for(f);
        if (!d->codec)
            return au_img_fail(d, d->nhead < sizeof d->head ? "truncated" : "not a known image format");
        d->info.format = f;
        d->st = d->codec->create(d);
        if (!d->st)
            return au_img_fail(d, "out of memory");
        const uint8_t *hp = d->head;
        size_t hn = d->nhead;
        st = run_codec(d, &hp, &hn, final && *in_len == 0);
        if (st != AU_IMAGE_MORE || hn) {
            /* The codec stopped inside the first 16 bytes (an error or a
             * header-only stop). */
            if (st == AU_IMAGE_MORE)
                st = au_img_fail(d, "corrupt image");
            update_mem(d);
            return st;
        }
    }
    st = run_codec(d, in, in_len, final);
    if (d->opt.header_only && d->info.ready && st != AU_IMAGE_ERROR) {
        d->done = true;
        st = AU_IMAGE_DONE;
    }
    update_mem(d);
    return st;
}

/* ---- Header and canvas ---- */

bool au_img_header(AuImageDecoder *d, int w, int h, int orient, bool alpha)
{
    if (w <= 0 || h <= 0)
        return au_img_fail(d, "zero size"), false;
    if (w > AU_IMAGE_MAX_DIM || h > AU_IMAGE_MAX_DIM)
        return au_img_fail(d, "image too large"), false;
    if ((uint64_t)w * (uint64_t)h > d->opt.max_source_pixels)
        return au_img_fail(d, "image has too many pixels"), false;
    if (orient < 1 || orient > 8)
        orient = 1;
    d->orient = orient;
    d->info.orientation = orient;
    d->info.width = orient >= 5 ? h : w;
    d->info.height = orient >= 5 ? w : h;
    d->info.has_alpha = alpha;
    int s = au_img_scale(d);
    d->info.scale_denom = s;
    d->info.out_w = (d->info.width + s - 1) / s;
    d->info.out_h = (d->info.height + s - 1) / s;
    d->info.ready = true;
    return true;
}

int au_img_scale(const AuImageDecoder *d)
{
    uint32_t W = (uint32_t)d->info.width, H = (uint32_t)d->info.height;
    if (!W || !H)
        return 1;
    uint32_t s = 1;
    uint32_t tw = (uint32_t)d->opt.target_w, th = (uint32_t)d->opt.target_h;
    if (tw || th) {
        while (s < 256 && (W + 2 * s - 1) / (2 * s) >= tw && (H + 2 * s - 1) / (2 * s) >= th &&
               (W + 2 * s - 1) / (2 * s) >= 1 && (H + 2 * s - 1) / (2 * s) >= 1)
            s *= 2;
    }
    while (s < 256 && (uint64_t)((W + s - 1) / s) * (uint64_t)((H + s - 1) / s) * 4 > d->opt.max_bytes)
        s *= 2;
    return (int)s;
}

bool au_img_begin(AuImageDecoder *d, int coded_w, int coded_h, int red)
{
    if (coded_w <= 0 || coded_h <= 0 || coded_w > AU_IMAGE_MAX_DIM || coded_h > AU_IMAGE_MAX_DIM)
        return au_img_fail(d, "bad size"), false;
    if (red < 1 || red > 256 || (red & (red - 1)))
        red = 1;
    if (d->canvas) {
        /* A codec that begins twice (a new frame size is not allowed). */
        if (coded_w != d->cw || coded_h != d->ch || red != d->red)
            return au_img_fail(d, "frame size changed"), false;
        return true;
    }
    int rw = (coded_w + red - 1) / red, rh = (coded_h + red - 1) / red;
    int ow = d->orient >= 5 ? rh : rw, oh = d->orient >= 5 ? rw : rh;
    size_t px, bytes;
    if (!au_img_mul((size_t)ow, (size_t)oh, &px) || !au_img_mul(px, 4, &bytes) || bytes > d->opt.max_bytes)
        return au_img_fail(d, "decoded image too large"), false;
    d->cw = coded_w;
    d->ch = coded_h;
    d->red = red;
    d->canvas = au_surface_new(ow, oh);
    if (red > 1)
        d->acc = au_calloc((size_t)rw * 4 * sizeof *d->acc);
    d->acc_n = 0;
    d->next_row = 0;
    d->info.out_w = ow;
    d->info.out_h = oh;
    if (d->info.width > 0 && ow > 0) {
        int s = (d->info.width + ow - 1) / ow;
        int p2 = 1;
        while (p2 < s && p2 < 256)
            p2 *= 2;
        d->info.scale_denom = p2;
    }
    d->info.ready = true;
    update_mem(d);
    return true;
}

void au_img_set_animation(AuImageDecoder *d, bool animated, int loop_count)
{
    d->info.animated = animated;
    d->info.loop_count = loop_count < 0 ? 0 : loop_count;
}

void au_img_mem(AuImageDecoder *d, ptrdiff_t delta)
{
    if (delta < 0 && (size_t)-delta > d->codec_bytes)
        d->codec_bytes = 0;
    else
        d->codec_bytes = (size_t)((ptrdiff_t)d->codec_bytes + delta);
    update_mem(d);
}

/* ---- Pixels ---- */

static inline uint32_t premul(uint32_t p)
{
    uint32_t a = p >> 24;
    if (a == 255)
        return p;
    if (a == 0)
        return 0;
    uint32_t rb = (p & 0x00FF00FFu) * a + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g = (p & 0x0000FF00u) * a + 0x00008000u;
    g = ((g + ((g >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
    return (a << 24) | rb | g;
}

static void mark(AuImageDecoder *d, int x0, int y0, int x1, int y1)
{
    if (x0 >= x1 || y0 >= y1)
        return;
    if (!d->any_dirty) {
        d->dirty = (AuIRect){x0, y0, x1, y1};
        d->any_dirty = true;
        return;
    }
    if (x0 < d->dirty.x0)
        d->dirty.x0 = x0;
    if (y0 < d->dirty.y0)
        d->dirty.y0 = y0;
    if (x1 > d->dirty.x1)
        d->dirty.x1 = x1;
    if (y1 > d->dirty.y1)
        d->dirty.y1 = y1;
}

/* Canvas position of reduced coded pixel (x, y). */
static inline void orient_xy(const AuImageDecoder *d, int x, int y, int rw, int rh, int *ox, int *oy)
{
    switch (d->orient) {
    default: *ox = x; *oy = y; break;
    case 2: *ox = rw - 1 - x; *oy = y; break;
    case 3: *ox = rw - 1 - x; *oy = rh - 1 - y; break;
    case 4: *ox = x; *oy = rh - 1 - y; break;
    case 5: *ox = y; *oy = x; break;
    case 6: *ox = rh - 1 - y; *oy = x; break;
    case 7: *ox = rh - 1 - y; *oy = rw - 1 - x; break;
    case 8: *ox = y; *oy = rw - 1 - x; break;
    }
}

/* Writes a reduced row ry of premultiplied pixels (rw of them). */
static void put_row(AuImageDecoder *d, int ry, const uint32_t *px, bool premultiplied)
{
    AuSurface *s = d->canvas;
    int rw = (d->cw + d->red - 1) / d->red, rh = (d->ch + d->red - 1) / d->red;
    if (ry < 0 || ry >= rh)
        return;
    if (d->orient == 1) {
        AuPixel *row = s->pixels + (size_t)ry * s->stride;
        if (premultiplied)
            memcpy(row, px, (size_t)rw * 4);
        else
            for (int x = 0; x < rw; x++)
                row[x] = premul(px[x]);
        mark(d, 0, ry, rw, ry + 1);
        return;
    }
    int ax, ay, bx, by;
    for (int x = 0; x < rw; x++) {
        int ox, oy;
        orient_xy(d, x, ry, rw, rh, &ox, &oy);
        s->pixels[(size_t)oy * s->stride + ox] = premultiplied ? px[x] : premul(px[x]);
    }
    orient_xy(d, 0, ry, rw, rh, &ax, &ay);
    orient_xy(d, rw - 1, ry, rw, rh, &bx, &by);
    mark(d, ax < bx ? ax : bx, ay < by ? ay : by, (ax > bx ? ax : bx) + 1, (ay > by ? ay : by) + 1);
}

static void flush_acc(AuImageDecoder *d, int ry)
{
    int red = d->red, rw = (d->cw + red - 1) / red;
    uint32_t *acc = d->acc;
    /* Reuse the accumulator as the output row: pixel i reads acc[4i..4i+3]
     * and writes acc[i], which is never ahead of the reads. */
    for (int x = 0; x < rw; x++) {
        int cols = d->cw - x * red;
        if (cols > red)
            cols = red;
        uint32_t n = (uint32_t)cols * (uint32_t)d->acc_n, h = n / 2;
        uint32_t a = (acc[4 * x] + h) / n, r = (acc[4 * x + 1] + h) / n, g = (acc[4 * x + 2] + h) / n,
                 b = (acc[4 * x + 3] + h) / n;
        if (r > a)
            r = a;
        if (g > a)
            g = a;
        if (b > a)
            b = a;
        acc[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    put_row(d, ry, acc, true);
    memset(acc, 0, (size_t)rw * 4 * sizeof *acc);
    d->acc_n = 0;
}

void au_img_row(AuImageDecoder *d, int y, const uint32_t *argb)
{
    if (!d->canvas || y < 0 || y >= d->ch)
        return;
    if (y != d->next_row) {
        au_img_row_at(d, y, 1, argb);
        return;
    }
    d->next_row = y + 1;
    if (d->red == 1) {
        put_row(d, y, argb, false);
        return;
    }
    int red = d->red;
    uint32_t *acc = d->acc;
    for (int x = 0; x < d->cw; x++) {
        uint32_t p = premul(argb[x]);
        uint32_t *a = acc + 4 * (x / red);
        a[0] += p >> 24;
        a[1] += (p >> 16) & 255;
        a[2] += (p >> 8) & 255;
        a[3] += p & 255;
    }
    d->acc_n++;
    if (d->acc_n == red || y == d->ch - 1)
        flush_acc(d, y / red);
}

void au_img_row_at(AuImageDecoder *d, int y, int fill, const uint32_t *argb)
{
    if (!d->canvas || y < 0 || y >= d->ch)
        return;
    if (fill < 1)
        fill = 1;
    if (y + 1 > d->next_row)
        d->next_row = y + 1; /* partial: some rows exist */
    int red = d->red, rw = (d->cw + red - 1) / red;
    uint32_t tmp[256];
    for (int yy = y; yy < y + fill && yy < d->ch; yy++) {
        if (yy % red)
            continue;
        if (red == 1) {
            put_row(d, yy, argb, false);
            continue;
        }
        /* Point sample, a block of pixels at a time. */
        AuSurface *s = d->canvas;
        int rh = (d->ch + red - 1) / red, ry = yy / red;
        for (int x0 = 0; x0 < rw; x0 += 256) {
            int n = rw - x0 < 256 ? rw - x0 : 256;
            for (int i = 0; i < n; i++)
                tmp[i] = premul(argb[(size_t)(x0 + i) * red]);
            for (int i = 0; i < n; i++) {
                int ox, oy;
                orient_xy(d, x0 + i, ry, rw, rh, &ox, &oy);
                s->pixels[(size_t)oy * s->stride + ox] = tmp[i];
            }
        }
        int ax, ay, bx, by;
        orient_xy(d, 0, ry, rw, rh, &ax, &ay);
        orient_xy(d, rw - 1, ry, rw, rh, &bx, &by);
        mark(d, ax < bx ? ax : bx, ay < by ? ay : by, (ax > bx ? ax : bx) + 1, (ay > by ? ay : by) + 1);
    }
}

static inline uint32_t over(uint32_t s, uint32_t d)
{
    uint32_t sa = s >> 24;
    if (sa == 255)
        return s;
    if (sa == 0)
        return d;
    uint32_t ia = 255 - sa;
    uint32_t rb = (d & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((d >> 8) & 0x00FF00FFu) * ia + 0x00800080u;
    ag = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    return s + (rb | (ag << 8));
}

void au_img_frame_row(AuImageDecoder *d, int x, int y, int w, const uint32_t *argb, int blend)
{
    if (!d->canvas || y < 0 || y >= d->ch || w <= 0)
        return;
    int red = d->red;
    if (y % red)
        return;
    /* Clip [x, x + w) to [0, cw); src index follows. */
    int i0 = 0;
    if (x < 0) {
        if (w <= -x)
            return;
        i0 = -x;
    }
    int end = x + w > d->cw ? d->cw - x : w;
    if (end <= i0)
        return;
    if (y + 1 > d->next_row)
        d->next_row = y + 1;
    AuSurface *s = d->canvas;
    /* Frames are drawn without orientation (only JPEG has one). */
    AuPixel *row = s->pixels + (size_t)(y / red) * s->stride;
    int first = -1, last = -1;
    for (int i = i0; i < end; i++) {
        int cx = x + i;
        if (cx % red)
            continue;
        int ox = cx / red;
        if (ox >= s->width)
            break;
        uint32_t p = argb[i];
        switch (blend) {
        case AU_IMG_BLEND_KEY:
            if (!(p >> 24))
                continue;
            row[ox] = premul(p);
            break;
        case AU_IMG_BLEND_OVER:
            row[ox] = over(premul(p), row[ox]);
            break;
        default:
            row[ox] = premul(p);
            break;
        }
        if (first < 0)
            first = ox;
        last = ox;
    }
    if (first >= 0)
        mark(d, first, y / red, last + 1, y / red + 1);
}

/* Maps a coded rectangle to the canvas pixels whose samples lie in it. */
static bool map_rect(const AuImageDecoder *d, int x, int y, int w, int h, AuIRect *r)
{
    if (!d->canvas || w <= 0 || h <= 0)
        return false;
    int red = d->red;
    long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    long x1 = (long)x + w, y1 = (long)y + h;
    if (x1 > d->cw)
        x1 = d->cw;
    if (y1 > d->ch)
        y1 = d->ch;
    if (x0 >= x1 || y0 >= y1)
        return false;
    r->x0 = (int)((x0 + red - 1) / red);
    r->y0 = (int)((y0 + red - 1) / red);
    r->x1 = (int)((x1 + red - 1) / red);
    r->y1 = (int)((y1 + red - 1) / red);
    if (r->x1 > d->canvas->width)
        r->x1 = d->canvas->width;
    if (r->y1 > d->canvas->height)
        r->y1 = d->canvas->height;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

void au_img_clear_rect(AuImageDecoder *d, int x, int y, int w, int h)
{
    AuIRect r;
    if (!map_rect(d, x, y, w, h, &r))
        return;
    for (int yy = r.y0; yy < r.y1; yy++)
        memset(d->canvas->pixels + (size_t)yy * d->canvas->stride + r.x0, 0, (size_t)(r.x1 - r.x0) * 4);
    mark(d, r.x0, r.y0, r.x1, r.y1);
}

uint32_t *au_img_save_rect(AuImageDecoder *d, int x, int y, int w, int h)
{
    AuIRect r;
    if (!map_rect(d, x, y, w, h, &r))
        return NULL;
    size_t rw = (size_t)(r.x1 - r.x0);
    uint32_t *buf = au_alloc(rw * (size_t)(r.y1 - r.y0) * 4);
    for (int yy = r.y0; yy < r.y1; yy++)
        memcpy(buf + (size_t)(yy - r.y0) * rw, d->canvas->pixels + (size_t)yy * d->canvas->stride + r.x0, rw * 4);
    return buf;
}

void au_img_restore_rect(AuImageDecoder *d, uint32_t *saved, int x, int y, int w, int h)
{
    AuIRect r;
    if (saved && map_rect(d, x, y, w, h, &r)) {
        size_t rw = (size_t)(r.x1 - r.x0);
        for (int yy = r.y0; yy < r.y1; yy++)
            memcpy(d->canvas->pixels + (size_t)yy * d->canvas->stride + r.x0, saved + (size_t)(yy - r.y0) * rw,
                   rw * 4);
        mark(d, r.x0, r.y0, r.x1, r.y1);
    }
    au_free(saved);
}

void au_img_frame_done(AuImageDecoder *d, uint32_t delay_ms)
{
    if (d->canvas) {
        /* Flush a partly accumulated row (a frame that ended early). */
        if (d->acc && d->acc_n)
            flush_acc(d, (d->next_row - 1) / d->red);
        bool opaque = true;
        AuSurface *s = d->canvas;
        for (int y = 0; y < s->height && opaque; y++) {
            const AuPixel *row = s->pixels + (size_t)y * s->stride;
            for (int x = 0; x < s->width; x++)
                if ((row[x] >> 24) != 255) {
                    opaque = false;
                    break;
                }
        }
        s->opaque = opaque;
    }
    d->info.frame_count++;
    d->info.frame_delay_ms = delay_ms;
    if (d->info.frame_count > 1)
        d->info.animated = true;
}

/* ---- Byte holding ---- */

bool au_img_hold(AuImgHold *h, size_t want, const uint8_t **in, size_t *len)
{
    if (h->len >= want)
        return true;
    if (h->cap < want) {
        h->buf = au_realloc(h->buf, want);
        h->cap = want;
    }
    size_t n = want - h->len;
    if (n > *len)
        n = *len;
    memcpy(h->buf + h->len, *in, n);
    h->len += n;
    *in += n;
    *len -= n;
    return h->len >= want;
}

void au_img_hold_free(AuImgHold *h)
{
    au_free(h->buf);
    h->buf = NULL;
    h->len = h->cap = 0;
}

/* ---- Whole buffers ---- */

AuSurface *au_image_decode_buffer(const void *data, size_t len, const AuImageOptions *opt, AuImageInfo *info)
{
    AuImageOptions o = opt ? *opt : (AuImageOptions){0};
    o.first_frame_only = true;
    AuImageDecoder *d = au_image_decoder_new(&o);
    const uint8_t *p = data;
    size_t n = len;
    AuImageStatus st = au_image_decode(d, &p, &n, true);
    AuSurface *s = NULL;
    if (st == AU_IMAGE_FRAME || (st == AU_IMAGE_ERROR && d->info.partial))
        s = au_image_take_canvas(d);
    if (info)
        *info = d->info;
    au_image_decoder_free(d);
    return s;
}

bool au_image_probe(const void *data, size_t len, AuImageInfo *info)
{
    AuImageOptions o = {0};
    o.header_only = true;
    AuImageDecoder *d = au_image_decoder_new(&o);
    const uint8_t *p = data;
    size_t n = len;
    au_image_decode(d, &p, &n, true);
    bool ok = d->info.ready;
    if (info)
        *info = d->info;
    au_image_decoder_free(d);
    return ok;
}
