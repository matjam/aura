/* WebP: the RIFF container, still images (simple VP8 or VP8L, and VP8X
 * extended files with ALPH), and animation (ANIM, ANMF).
 *
 * The container is parsed as a byte-at-a-time state machine. Chunk
 * headers are held in a small buffer; ICCP, EXIF, XMP and unknown chunks
 * are skipped without storing them. The payload of an ALPH, VP8 or VP8L
 * chunk is buffered (it grows as bytes arrive, up to the chunk's
 * declared size, which may not exceed WEBP_MAX_CHUNK or the RIFF size)
 * and decoded when complete; the bitstream decoders emit rows as they
 * decode. When the input ends inside an image chunk, the part that
 * arrived is decoded (rows that decode completely are kept) and the
 * decode fails as truncated.
 *
 * Animation: each ANMF frame is composited at its offset with its blend
 * mode after the previous frame's disposal; the canvas starts transparent
 * and disposal clears to transparent (the ANIM background colour is
 * ignored, as browsers do). */
#include "webp_int.h"

#define WEBP_MAX_CHUNK ((uint32_t)32 * 1024 * 1024)

enum { ST_RIFF, ST_CHUNK, ST_VP8X, ST_ANIM, ST_ANMF, ST_BUFFER, ST_SKIP, ST_DONE };
enum { K_ALPH, K_VP8, K_VP8L };

typedef struct {
    int state;
    uint32_t pos, riff_end; /* stream offsets */
    AuImgHold hold;
    bool first_chunk_seen;

    /* The current chunk. */
    uint32_t cend, cpad; /* payload end; end with padding */
    uint32_t skip_to;
    int kind;

    /* VP8X / canvas. */
    bool vp8x, animated, alpha_flag, anim_seen;
    int cw, ch;
    bool begun;

    /* The current ANMF frame. */
    bool in_frame;
    uint32_t frame_end, frame_pad;
    int fx, fy, fw, fh;
    uint32_t duration;
    bool blend, dispose_bg;
    /* The previous frame's disposal. */
    bool prev_dispose;
    int px, py, pw, ph;
    uint32_t frames;

    /* Buffered payloads. */
    uint8_t *buf;
    size_t buf_len, buf_cap;
    uint8_t *alph; /* the frame's ALPH payload */
    size_t alph_len;
    size_t accounted;
} WpState;

typedef struct {
    AuImageDecoder *d;
    WpState *w;
    const uint8_t *alpha; /* alpha plane (VP8 with ALPH), or NULL */
    int iw;
    bool anim;
    int fx, fy, blend;
} WpEmit;

static void w_account(WpState *w, AuImageDecoder *d, ptrdiff_t n)
{
    w->accounted += (size_t)n;
    au_img_mem(d, n);
}

static void emit_row(void *ctx, int y, uint32_t *row)
{
    WpEmit *e = ctx;
    if (e->alpha) {
        const uint8_t *a = e->alpha + (size_t)y * (size_t)e->iw;
        for (int x = 0; x < e->iw; x++)
            row[x] = (row[x] & 0x00ffffffu) | ((uint32_t)a[x] << 24);
    }
    if (e->anim)
        au_img_frame_row(e->d, e->fx, e->fy + y, e->iw, row, e->blend);
    else
        au_img_row(e->d, y, row);
}

static uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

/* Reverses the ALPH filter in place, row by row (libwebp's unfilters). */
static void alpha_unfilter(uint8_t *a, int w, int h, int filter)
{
    for (int y = 0; y < h; y++) {
        uint8_t *row = a + (size_t)y * (size_t)w;
        const uint8_t *prev = y ? row - w : NULL;
        if (filter == 1 || !prev) { /* horizontal, and the first row of all */
            uint8_t pred = prev ? prev[0] : 0;
            for (int x = 0; x < w; x++) {
                row[x] = (uint8_t)(pred + row[x]);
                pred = row[x];
            }
        } else if (filter == 2) { /* vertical */
            for (int x = 0; x < w; x++)
                row[x] = (uint8_t)(prev[x] + row[x]);
        } else { /* gradient */
            int top = prev[0], tl = top, left = top;
            for (int x = 0; x < w; x++) {
                top = prev[x];
                int g = left + top - tl;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                left = (uint8_t)(row[x] + g);
                tl = top;
                row[x] = (uint8_t)left;
            }
        }
    }
}

/* Decodes the frame's ALPH payload into a w x h plane (NULL on error,
 * with *err set). */
static uint8_t *decode_alpha(WpState *w, AuImageDecoder *d, int iw, int ih, const char **err)
{
    const uint8_t *p = w->alph;
    size_t n = w->alph_len;
    *err = "bad alpha";
    if (n < 1)
        return NULL;
    int method = p[0] & 3, filter = (p[0] >> 2) & 3, pre = (p[0] >> 4) & 3, rsrv = p[0] >> 6;
    if (method > 1 || pre > 1 || rsrv)
        return NULL;
    size_t px;
    if (!au_img_mul((size_t)iw, (size_t)ih, &px) || px > au_webp_pixel_cap(d)) {
        *err = "alpha too large";
        return NULL;
    }
    if (method == 0 && n - 1 < px)
        return NULL;
    uint8_t *a = au_alloc(px);
    w_account(w, d, (ptrdiff_t)px);
    if (method == 0) {
        memcpy(a, p + 1, px);
    } else {
        const char *e = au_webp_vp8l_decode_alpha(d, p + 1, n - 1, iw, ih, a);
        if (e) {
            *err = e;
            au_free(a);
            w_account(w, d, -(ptrdiff_t)px);
            return NULL;
        }
    }
    if (filter)
        alpha_unfilter(a, iw, ih, filter);
    *err = NULL;
    return a;
}

static void free_buf(WpState *w, AuImageDecoder *d)
{
    au_free(w->buf);
    w_account(w, d, -(ptrdiff_t)w->buf_cap);
    w->buf = NULL;
    w->buf_len = w->buf_cap = 0;
}

static void free_alph(WpState *w, AuImageDecoder *d)
{
    au_free(w->alph);
    w_account(w, d, -(ptrdiff_t)w->alph_len);
    w->alph = NULL;
    w->alph_len = 0;
}

/* Gives the header once the canvas size is known. */
static bool give_header(WpState *w, AuImageDecoder *d, int cw, int ch, bool alpha)
{
    if (d->info.ready)
        return true;
    w->cw = cw;
    w->ch = ch;
    return au_img_header(d, cw, ch, 1, alpha);
}

/* For a simple file, gives the header as soon as the bitstream's own
 * header has arrived. */
static bool peek_header(WpState *w, AuImageDecoder *d)
{
    if (w->vp8x || d->info.ready)
        return true;
    int iw, ih;
    bool alpha = false;
    if (w->kind == K_VP8 && w->buf_len >= 10) {
        if (!au_webp_vp8_size(w->buf, w->buf_len, &iw, &ih))
            return au_img_fail(d, "bad VP8 header"), false;
        return give_header(w, d, iw, ih, false);
    }
    if (w->kind == K_VP8L && w->buf_len >= 5) {
        if (!au_webp_vp8l_size(w->buf, w->buf_len, &iw, &ih, &alpha))
            return au_img_fail(d, "bad VP8L header"), false;
        return give_header(w, d, iw, ih, alpha);
    }
    return true;
}

/* Decodes the buffered VP8 or VP8L payload as the next frame (truncated:
 * the input ended inside it). */
static AuImageStatus decode_image(WpState *w, AuImageDecoder *d, bool truncated)
{
    int iw, ih;
    bool alpha = false;
    bool ok = w->kind == K_VP8 ? au_webp_vp8_size(w->buf, w->buf_len, &iw, &ih)
                               : au_webp_vp8l_size(w->buf, w->buf_len, &iw, &ih, &alpha);
    if (!ok)
        return au_img_fail(d, truncated ? "truncated" : "bad image header");
    if (!give_header(w, d, iw, ih, alpha))
        return AU_IMAGE_ERROR;
    if (w->in_frame) {
        if (iw != w->fw || ih != w->fh)
            return au_img_fail(d, "frame size mismatch");
    } else if (iw != w->cw || ih != w->ch) {
        return au_img_fail(d, "image size mismatch");
    }
    if (!w->begun) {
        if (!au_img_begin(d, w->cw, w->ch, au_img_scale(d)))
            return AU_IMAGE_ERROR;
        w->begun = true;
    }
    WpEmit e = {d, w, NULL, iw, w->animated, 0, 0, AU_IMG_BLEND_SRC};
    if (w->animated) {
        if (w->prev_dispose)
            au_img_clear_rect(d, w->px, w->py, w->pw, w->ph);
        e.fx = w->fx;
        e.fy = w->fy;
        e.blend = w->blend ? AU_IMG_BLEND_OVER : AU_IMG_BLEND_SRC;
    }
    uint8_t *plane = NULL;
    const char *err = NULL;
    if (w->kind == K_VP8 && w->alph) {
        plane = decode_alpha(w, d, iw, ih, &err);
        if (!plane)
            return au_img_fail(d, err);
        e.alpha = plane;
    }
    if (w->kind == K_VP8)
        err = au_webp_vp8_decode(d, w->buf, w->buf_len, truncated, emit_row, &e);
    else
        err = au_webp_vp8l_decode(d, w->buf, w->buf_len, emit_row, &e);
    if (plane) {
        au_free(plane);
        w_account(w, d, -(ptrdiff_t)((size_t)iw * (size_t)ih));
    }
    free_buf(w, d);
    free_alph(w, d);
    if (err || truncated)
        return au_img_fail(d, truncated ? "truncated" : err);
    w->frames++;
    au_img_frame_done(d, w->animated ? w->duration : 0);
    if (w->animated) {
        w->prev_dispose = w->dispose_bg;
        w->px = w->fx;
        w->py = w->fy;
        w->pw = w->fw;
        w->ph = w->fh;
        /* Skip the rest of the ANMF payload. */
        w->in_frame = false;
        w->skip_to = w->frame_pad;
        w->state = ST_SKIP;
    } else {
        w->state = ST_DONE;
    }
    return AU_IMAGE_FRAME;
}

static void *create(AuImageDecoder *d)
{
    (void)d;
    return au_calloc(sizeof(WpState));
}

static void destroy(void *st)
{
    WpState *w = st;
    au_img_hold_free(&w->hold);
    au_free(w->buf);
    au_free(w->alph);
    au_free(w);
}

/* Starts the chunk whose header is in the hold buffer. */
static AuImageStatus start_chunk(WpState *w, AuImageDecoder *d)
{
    const uint8_t *h = w->hold.buf;
    uint32_t size = au_img_le32(h + 4);
    uint32_t limit = w->in_frame ? w->frame_end : w->riff_end;
    w->hold.len = 0;
    w->pos += 8;
    if (size > limit - w->pos)
        return au_img_fail(d, "bad chunk size");
    w->cend = w->pos + size;
    w->cpad = w->cend + (size & 1);
    if (w->cpad > limit)
        w->cpad = limit; /* a missing pad byte at the very end */
    bool first = !w->first_chunk_seen;
    w->first_chunk_seen = true;
    w->skip_to = w->cpad;
    if (!memcmp(h, "VP8X", 4)) {
        if (!first || size < 10)
            return au_img_fail(d, "bad VP8X");
        w->state = ST_VP8X;
        return AU_IMAGE_MORE;
    }
    bool image = !memcmp(h, "VP8 ", 4), lossless = !memcmp(h, "VP8L", 4), alph = !memcmp(h, "ALPH", 4);
    if (first && !image && !lossless)
        return au_img_fail(d, "not a WebP image");
    if (!memcmp(h, "ANIM", 4) && w->animated && !w->in_frame) {
        if (size < 6)
            return au_img_fail(d, "bad ANIM");
        w->state = ST_ANIM;
        return AU_IMAGE_MORE;
    }
    if (!memcmp(h, "ANMF", 4)) {
        if (!w->animated || w->in_frame || !w->anim_seen || size < 16)
            return au_img_fail(d, "bad ANMF");
        w->state = ST_ANMF;
        return AU_IMAGE_MORE;
    }
    if (image || lossless || alph) {
        if (w->animated && !w->in_frame)
            return au_img_fail(d, "image outside a frame");
        if (alph && (!w->vp8x || w->alph)) {
            w->state = ST_SKIP; /* ALPH in a simple file, or a second one */
            return AU_IMAGE_MORE;
        }
        if (size > WEBP_MAX_CHUNK)
            return au_img_fail(d, "chunk too large");
        w->kind = image ? K_VP8 : lossless ? K_VP8L : K_ALPH;
        w->state = ST_BUFFER;
        return AU_IMAGE_MORE;
    }
    w->state = ST_SKIP; /* ICCP, EXIF, XMP, unknown */
    return AU_IMAGE_MORE;
}

static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    WpState *w = st;
    for (;;) {
        switch (w->state) {
        case ST_RIFF: {
            if (!au_img_hold(&w->hold, 12, in, len))
                return AU_IMAGE_MORE;
            const uint8_t *h = w->hold.buf;
            if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WEBP", 4))
                return au_img_fail(d, "not a WebP image");
            uint32_t size = au_img_le32(h + 4);
            if (size < 12 || size > 0xfffffff6u)
                return au_img_fail(d, "bad RIFF size");
            w->riff_end = 8 + size;
            w->pos = 12;
            w->hold.len = 0;
            w->state = ST_CHUNK;
            break;
        }
        case ST_CHUNK: {
            if (w->in_frame && w->pos >= w->frame_end)
                return au_img_fail(d, "frame without image");
            if (w->pos >= w->riff_end) {
                if (w->animated && w->frames > 0)
                    return AU_IMAGE_DONE;
                return au_img_fail(d, "no image");
            }
            uint32_t limit = w->in_frame ? w->frame_end : w->riff_end;
            if (limit - w->pos < 8)
                return au_img_fail(d, "bad chunk");
            if (!au_img_hold(&w->hold, 8, in, len))
                return AU_IMAGE_MORE;
            AuImageStatus s = start_chunk(w, d);
            if (s == AU_IMAGE_ERROR)
                return s;
            break;
        }
        case ST_VP8X: {
            if (!au_img_hold(&w->hold, 10, in, len))
                return AU_IMAGE_MORE;
            const uint8_t *h = w->hold.buf;
            w->vp8x = true;
            w->animated = (h[0] & 2) != 0;
            w->alpha_flag = (h[0] & 16) != 0;
            int cw = (int)le24(h + 4) + 1, ch = (int)le24(h + 7) + 1;
            w->pos += 10;
            w->hold.len = 0;
            if (!give_header(w, d, cw, ch, w->alpha_flag || w->animated))
                return AU_IMAGE_ERROR;
            w->state = ST_SKIP;
            break;
        }
        case ST_ANIM: {
            if (!au_img_hold(&w->hold, 6, in, len))
                return AU_IMAGE_MORE;
            /* Bytes 0-3: background colour, ignored. */
            au_img_set_animation(d, true, (int)au_img_le16(w->hold.buf + 4));
            w->anim_seen = true;
            w->pos += 6;
            w->hold.len = 0;
            w->state = ST_SKIP;
            break;
        }
        case ST_ANMF: {
            if (!au_img_hold(&w->hold, 16, in, len))
                return AU_IMAGE_MORE;
            const uint8_t *h = w->hold.buf;
            w->fx = 2 * (int)le24(h);
            w->fy = 2 * (int)le24(h + 3);
            w->fw = (int)le24(h + 6) + 1;
            w->fh = (int)le24(h + 9) + 1;
            w->duration = le24(h + 12);
            w->blend = !(h[15] & 2);
            w->dispose_bg = (h[15] & 1) != 0;
            w->pos += 16;
            w->hold.len = 0;
            if (w->fx + w->fw > w->cw || w->fy + w->fh > w->ch)
                return au_img_fail(d, "frame outside the canvas");
            w->in_frame = true;
            w->frame_end = w->cend;
            w->frame_pad = w->cpad;
            w->state = ST_CHUNK;
            break;
        }
        case ST_BUFFER: {
            size_t want = w->cend - w->pos, n = *len < want ? *len : want;
            if (n) {
                if (w->buf_len + n > w->buf_cap) {
                    size_t cap = w->buf_cap ? w->buf_cap * 2 : 4096;
                    while (cap < w->buf_len + n)
                        cap *= 2;
                    size_t total = w->cend - w->pos + w->buf_len;
                    if (cap > total)
                        cap = total;
                    w->buf = au_realloc(w->buf, cap);
                    w_account(w, d, (ptrdiff_t)(cap - w->buf_cap));
                    w->buf_cap = cap;
                }
                memcpy(w->buf + w->buf_len, *in, n);
                w->buf_len += n;
                *in += n;
                *len -= n;
                w->pos += (uint32_t)n;
                if (!peek_header(w, d))
                    return AU_IMAGE_ERROR;
            }
            if (w->pos < w->cend) {
                if (final && w->kind != K_ALPH && w->buf_len)
                    return decode_image(w, d, true);
                return AU_IMAGE_MORE;
            }
            if (w->kind == K_ALPH) {
                w->alph = w->buf;
                w->alph_len = w->buf_len;
                /* Keep the accounting: the bytes now belong to alph. */
                w_account(w, d, -(ptrdiff_t)(w->buf_cap - w->buf_len));
                w->buf = NULL;
                w->buf_len = w->buf_cap = 0;
                w->state = ST_SKIP;
                break;
            }
            /* Skip the pad byte later; decode now. */
            w->skip_to = w->cpad;
            return decode_image(w, d, false);
        }
        case ST_SKIP: {
            size_t want = w->skip_to - w->pos, n = *len < want ? *len : want;
            *in += n;
            *len -= n;
            w->pos += (uint32_t)n;
            if (w->pos < w->skip_to)
                return AU_IMAGE_MORE;
            w->state = ST_CHUNK;
            break;
        }
        default: return AU_IMAGE_DONE;
        }
    }
}

const AuImgCodec au_img_webp_codec = {create, run, destroy};
