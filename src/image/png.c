/* PNG and APNG.
 *
 * A chunk-level state machine: small chunks (IHDR, PLTE, tRNS, acTL,
 * fcTL) are held whole and parsed after their CRC is checked; IDAT and
 * fdAT data stream through inflate (src/codec, zlib framing) into a
 * scanline buffer, so rows are emitted as their bytes arrive; other
 * chunks are skipped (their CRC is still checked, and a bad one in an
 * ancillary chunk only drops that chunk). Every colour type and bit
 * depth, palette and tRNS transparency, and Adam7 interlacing are
 * supported. Gamma, colour profiles and text chunks are ignored: pixels
 * are shown as stored, as browsers do for untagged images. 16-bit
 * samples keep their high byte.
 *
 * Interlaced still images fill in progressively: each pass's pixel is
 * drawn over the block it stands for until later passes refine it.
 *
 * APNG: with an acTL before the first IDAT, frames (the IDAT image when
 * an fcTL precedes it, then each fcTL + fdAT sequence) are composited
 * with their offsets, blend and dispose operations. A default image
 * that is not part of the animation is decoded but not shown. */
#include "image_int.h"

#include "../codec/codec.h"

enum { S_SIG, S_HDR, S_HOLD, S_SEQ, S_DATA, S_SKIP, S_CRC, S_END };

/* Adam7: x0, y0, dx, dy, and the block a pass pixel stands for. */
static const uint8_t adam7[7][6] = {
    {0, 0, 8, 8, 8, 8}, {4, 0, 8, 8, 4, 8}, {0, 4, 4, 8, 4, 4}, {2, 0, 4, 4, 2, 4},
    {0, 2, 2, 4, 2, 2}, {1, 0, 2, 2, 1, 2}, {0, 1, 1, 2, 1, 1},
};

#define MAX_HOLD 1024
#define MAX_FRAMES 100000u

typedef struct {
    int state;
    uint8_t hdr[8];
    size_t nhdr;
    uint32_t clen, cleft; /* chunk length, bytes of data left */
    uint32_t ctype;
    uint32_t crc;
    uint8_t crcbuf[4];
    size_t ncrc;
    AuImgHold hold;

    /* IHDR. */
    bool have_ihdr;
    int w, h, depth, ctype_px, interlace;
    int channels, bpp; /* samples per pixel; bytes per complete pixel (at least 1) */
    /* Palette and transparency. */
    uint32_t pal[256];
    int npal;
    bool have_trns;
    uint16_t trns_key[3];
    bool seen_idat, seen_plte;

    /* APNG. */
    bool apng;
    uint32_t nframes_decl, frames_done;
    uint32_t next_seq;
    bool have_fctl;      /* an fcTL is waiting for its data */
    bool idat_is_frame;  /* the IDAT image is frame 0 */
    int fx, fy, fw, fh;  /* the current frame */
    uint32_t delay;
    int dispose, blend;
    /* The previous frame, for its disposal. */
    bool prev_valid;
    int px, py, pw, ph, pdispose;
    uint32_t *saved;

    /* Image data of the current frame. */
    bool in_frame;  /* rows are being decoded */
    bool stream_open; /* the frame's zlib stream has not ended */
    bool hidden;    /* the default image outside the animation */
    bool default_drawn; /* it is in the canvas */
    bool held;      /* the chunk's data is in hold, to use after its CRC */
    bool stream_is_idat; /* the open stream is the IDAT image's */
    bool idat_done;      /* the IDAT run has ended */
    bool rows_done; /* all rows of the frame are in */
    bool frame_pending; /* rows_done; FRAME not yet returned */
    AuInflate *z;
    bool z_done;
    size_t excess; /* bytes inflated after the last row */
    int pass, pass_w, pass_h, pass_y;
    size_t rowbytes; /* this pass, without the filter byte */
    size_t rowfill;
    uint8_t *cur, *prev; /* rowbytes + 1 each (cur[0] is the filter byte) */
    size_t rowcap;
    uint32_t *argb;      /* fw pixels */
    uint32_t *block;     /* 8 pixels, a pass pixel repeated */
    bool begun;
} Png;

static void *create(AuImageDecoder *d)
{
    (void)d;
    Png *p = au_calloc(sizeof *p);
    p->state = S_SIG;
    return p;
}

static void destroy(void *st)
{
    Png *p = st;
    au_img_hold_free(&p->hold);
    if (p->z)
        au_inflate_free(p->z);
    au_free(p->cur);
    au_free(p->prev);
    au_free(p->argb);
    au_free(p->saved);
    au_free(p);
}

#define T(a, b, c, e) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(e))
#define T_IHDR T('I', 'H', 'D', 'R')
#define T_PLTE T('P', 'L', 'T', 'E')
#define T_IDAT T('I', 'D', 'A', 'T')
#define T_IEND T('I', 'E', 'N', 'D')
#define T_tRNS T('t', 'R', 'N', 'S')
#define T_acTL T('a', 'c', 'T', 'L')
#define T_fcTL T('f', 'c', 'T', 'L')
#define T_fdAT T('f', 'd', 'A', 'T')

static bool critical(uint32_t t) { return !(t & 0x20000000u); }

/* ---- Rows ---- */

static void set_pass(Png *p, int pass)
{
    p->pass = pass;
    p->pass_y = 0;
    if (p->interlace) {
        const uint8_t *a = adam7[pass];
        p->pass_w = p->fw > a[0] ? (p->fw - a[0] + a[2] - 1) / a[2] : 0;
        p->pass_h = p->fh > a[1] ? (p->fh - a[1] + a[3] - 1) / a[3] : 0;
    } else {
        p->pass_w = p->fw;
        p->pass_h = p->fh;
    }
    /* Fits: pass_w <= 32768, channels * depth <= 64. */
    p->rowbytes = ((size_t)p->pass_w * (size_t)(p->channels * p->depth) + 7) / 8;
    p->rowfill = 0;
    if (p->prev)
        memset(p->prev, 0, p->rowcap);
}

/* Moves to the next non-empty pass; false when the frame is complete. */
static bool next_pass(Png *p)
{
    for (;;) {
        if (!p->interlace || p->pass >= 6)
            return false;
        set_pass(p, p->pass + 1);
        if (p->pass_w > 0 && p->pass_h > 0)
            return true;
    }
}

static bool start_frame_rows(Png *p, AuImageDecoder *d)
{
    size_t rb;
    if (!au_img_mul((size_t)p->fw, (size_t)(p->channels * p->depth), &rb))
        return au_img_fail(d, "row too large"), false;
    rb = (rb + 7) / 8 + 1;
    if (rb > p->rowcap) {
        au_img_mem(d, (ptrdiff_t)(2 * (rb - p->rowcap)));
        p->cur = au_realloc(p->cur, rb);
        p->prev = au_realloc(p->prev, rb);
        p->rowcap = rb;
    }
    memset(p->prev, 0, p->rowcap);
    if (!p->argb) {
        /* The canvas width bounds every frame width. */
        au_img_mem(d, (ptrdiff_t)((size_t)p->w * 4 + 32));
        p->argb = au_alloc((size_t)p->w * 4);
        p->block = au_alloc(8 * 4);
    }
    if (!p->z) {
        p->z = au_inflate_new(AU_INFLATE_ZLIB);
        au_img_mem(d, 46 * 1024);
    } else {
        au_inflate_reset(p->z);
    }
    p->z_done = false;
    p->excess = 0;
    p->rows_done = false;
    p->in_frame = true;
    set_pass(p, 0);
    if (p->interlace && (p->pass_w == 0 || p->pass_h == 0) && !next_pass(p))
        p->rows_done = true;
    return true;
}

static inline int paeth(int a, int b, int c)
{
    int pp = a + b - c;
    int pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static bool unfilter(Png *p)
{
    uint8_t *r = p->cur + 1;
    const uint8_t *q = p->prev + 1;
    size_t n = p->rowbytes, bpp = (size_t)p->bpp, i;
    switch (p->cur[0]) {
    case 0:
        break;
    case 1:
        for (i = bpp; i < n; i++)
            r[i] = (uint8_t)(r[i] + r[i - bpp]);
        break;
    case 2:
        for (i = 0; i < n; i++)
            r[i] = (uint8_t)(r[i] + q[i]);
        break;
    case 3:
        for (i = 0; i < bpp && i < n; i++)
            r[i] = (uint8_t)(r[i] + (q[i] >> 1));
        for (; i < n; i++)
            r[i] = (uint8_t)(r[i] + ((r[i - bpp] + q[i]) >> 1));
        break;
    case 4:
        for (i = 0; i < bpp && i < n; i++)
            r[i] = (uint8_t)(r[i] + q[i]);
        for (; i < n; i++)
            r[i] = (uint8_t)(r[i] + paeth(r[i - bpp], q[i], q[i - bpp]));
        break;
    default:
        return false;
    }
    return true;
}

/* Converts the unfiltered row (pass_w pixels) to ARGB. */
static void convert(Png *p)
{
    const uint8_t *r = p->cur + 1;
    uint32_t *o = p->argb;
    int n = p->pass_w, depth = p->depth;
    bool trns = p->have_trns;
    switch (p->ctype_px) {
    case 0: /* grey */
        if (depth == 16) {
            for (int x = 0; x < n; x++) {
                uint32_t v = r[2 * x], raw = (uint32_t)(r[2 * x] << 8 | r[2 * x + 1]);
                uint32_t a = trns && raw == p->trns_key[0] ? 0 : 255;
                o[x] = a << 24 | v << 16 | v << 8 | v;
            }
        } else {
            int mul = depth == 1 ? 255 : depth == 2 ? 85 : depth == 4 ? 17 : 1;
            int per = 8 / depth, mask = (1 << depth) - 1;
            for (int x = 0; x < n; x++) {
                int raw = (r[x / per] >> ((per - 1 - x % per) * depth)) & mask;
                uint32_t v = (uint32_t)(raw * mul);
                uint32_t a = trns && raw == p->trns_key[0] ? 0 : 255;
                o[x] = a << 24 | v << 16 | v << 8 | v;
            }
        }
        break;
    case 2: /* RGB */
        if (depth == 16) {
            for (int x = 0; x < n; x++) {
                const uint8_t *s = r + 6 * x;
                uint32_t a = 255;
                if (trns && (uint32_t)(s[0] << 8 | s[1]) == p->trns_key[0] &&
                    (uint32_t)(s[2] << 8 | s[3]) == p->trns_key[1] && (uint32_t)(s[4] << 8 | s[5]) == p->trns_key[2])
                    a = 0;
                o[x] = a << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[2] << 8 | s[4];
            }
        } else {
            for (int x = 0; x < n; x++) {
                const uint8_t *s = r + 3 * x;
                uint32_t a = trns && s[0] == p->trns_key[0] && s[1] == p->trns_key[1] && s[2] == p->trns_key[2] ? 0 : 255;
                o[x] = a << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
            }
        }
        break;
    case 3: { /* palette; out-of-range indices are opaque black */
        int per = 8 / depth, mask = (1 << depth) - 1;
        for (int x = 0; x < n; x++) {
            int i = depth == 8 ? r[x] : (r[x / per] >> ((per - 1 - x % per) * depth)) & mask;
            o[x] = i < p->npal ? p->pal[i] : 0xFF000000u;
        }
        break;
    }
    case 4: /* grey + alpha */
        if (depth == 16) {
            for (int x = 0; x < n; x++) {
                uint32_t v = r[4 * x];
                o[x] = (uint32_t)r[4 * x + 2] << 24 | v << 16 | v << 8 | v;
            }
        } else {
            for (int x = 0; x < n; x++) {
                uint32_t v = r[2 * x];
                o[x] = (uint32_t)r[2 * x + 1] << 24 | v << 16 | v << 8 | v;
            }
        }
        break;
    default: /* 6: RGBA */
        if (depth == 16) {
            for (int x = 0; x < n; x++) {
                const uint8_t *s = r + 8 * x;
                o[x] = (uint32_t)s[6] << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[2] << 8 | s[4];
            }
        } else {
            for (int x = 0; x < n; x++) {
                const uint8_t *s = r + 4 * x;
                o[x] = (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
            }
        }
        break;
    }
}

static void emit_row(Png *p, AuImageDecoder *d)
{
    bool anim = p->apng;
    if (!p->interlace) {
        int y = p->pass_y;
        if (anim)
            au_img_frame_row(d, p->fx, p->fy + y, p->fw, p->argb, p->blend ? AU_IMG_BLEND_OVER : AU_IMG_BLEND_SRC);
        else
            au_img_row(d, y, p->argb);
        return;
    }
    const uint8_t *a = adam7[p->pass];
    int y = a[1] + p->pass_y * a[3];
    if (anim) {
        int blend = p->blend ? AU_IMG_BLEND_OVER : AU_IMG_BLEND_SRC;
        for (int i = 0; i < p->pass_w; i++)
            au_img_frame_row(d, p->fx + a[0] + i * a[2], p->fy + y, 1, &p->argb[i], blend);
        return;
    }
    /* A still image: each pixel covers its block until later passes. */
    int bw = a[4], bh = a[5];
    for (int i = 0; i < p->pass_w; i++) {
        int x = a[0] + i * a[2];
        for (int k = 0; k < bw; k++)
            p->block[k] = p->argb[i];
        for (int yy = y; yy < y + bh && yy < p->fh; yy++)
            au_img_frame_row(d, x, yy, bw, p->block, AU_IMG_BLEND_SRC);
    }
}

/* Feeds image data; returns false after failing. Sets frame_pending when
 * the last row is in. *consumed is how much of data was used. */
static bool feed_data(Png *p, AuImageDecoder *d, const uint8_t *data, size_t n, size_t *consumed)
{
    const uint8_t *in = data;
    size_t inl = n;
    while (inl && !p->z_done) {
        if (p->rows_done) {
            /* Trailing data (the Adler-32, or excess image data). */
            uint8_t sink[256];
            uint8_t *o = sink;
            size_t ol = sizeof sink;
            AuCodecStatus cs = au_inflate_run(p->z, &in, &inl, &o, &ol);
            p->excess += sizeof sink - ol;
            if (cs == AU_CODEC_DONE || p->excess > 65536) /* stop inflating excess data */
                p->z_done = true;
            else if (cs == AU_CODEC_ERROR)
                p->z_done = true; /* the image is complete; a bad trailer is ignored */
            continue;
        }
        uint8_t *o = p->cur + p->rowfill;
        size_t want = p->rowbytes + 1 - p->rowfill, ol = want;
        AuCodecStatus cs = au_inflate_run(p->z, &in, &inl, &o, &ol);
        p->rowfill += want - ol;
        if (cs == AU_CODEC_ERROR)
            return au_img_fail(d, "corrupt image data"), false;
        if (p->rowfill == p->rowbytes + 1) {
            if (!unfilter(p))
                return au_img_fail(d, "bad filter type"), false;
            convert(p);
            emit_row(p, d);
            uint8_t *t = p->prev;
            p->prev = p->cur;
            p->cur = t;
            p->rowfill = 0;
            if (++p->pass_y >= p->pass_h && !next_pass(p)) {
                p->rows_done = true;
                p->frame_pending = true;
                if (cs == AU_CODEC_DONE)
                    p->z_done = true;
                break;
            }
        }
        if (cs == AU_CODEC_DONE) {
            p->z_done = true;
            if (!p->rows_done)
                return au_img_fail(d, "not enough image data"), false;
        }
    }
    *consumed = n - inl;
    if (p->z_done)
        p->stream_open = false;
    return true;
}

/* ---- Chunks ---- */

static bool parse_ihdr(Png *p, AuImageDecoder *d, const uint8_t *c)
{
    uint32_t w = au_img_be32(c), h = au_img_be32(c + 4);
    int depth = c[8], ct = c[9];
    if (c[10] != 0 || c[11] != 0 || c[12] > 1)
        return au_img_fail(d, "bad IHDR"), false;
    bool ok;
    switch (ct) {
    case 0: ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; p->channels = 1; break;
    case 3: ok = depth == 1 || depth == 2 || depth == 4 || depth == 8; p->channels = 1; break;
    case 2: ok = depth == 8 || depth == 16; p->channels = 3; break;
    case 4: ok = depth == 8 || depth == 16; p->channels = 2; break;
    case 6: ok = depth == 8 || depth == 16; p->channels = 4; break;
    default: ok = false; break;
    }
    if (!ok)
        return au_img_fail(d, "bad colour type or depth"), false;
    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu)
        return au_img_fail(d, "bad size"), false;
    if (w > AU_IMAGE_MAX_DIM || h > AU_IMAGE_MAX_DIM)
        return au_img_fail(d, "image too large"), false;
    p->w = (int)w;
    p->h = (int)h;
    p->depth = depth;
    p->ctype_px = ct;
    p->interlace = c[12];
    p->bpp = p->channels * depth / 8;
    if (p->bpp < 1)
        p->bpp = 1;
    p->have_ihdr = true;
    return au_img_header(d, p->w, p->h, 1, ct == 4 || ct == 6);
}

static bool parse_fctl(Png *p, AuImageDecoder *d, const uint8_t *c)
{
    uint32_t seq = au_img_be32(c), w = au_img_be32(c + 4), h = au_img_be32(c + 8), x = au_img_be32(c + 12),
             y = au_img_be32(c + 16);
    uint32_t num = au_img_be16(c + 20), den = au_img_be16(c + 22);
    if (seq != p->next_seq)
        return au_img_fail(d, "APNG sequence error"), false;
    p->next_seq++;
    if (!w || !h || w > (uint32_t)p->w || h > (uint32_t)p->h || x > (uint32_t)p->w - w || y > (uint32_t)p->h - h)
        return au_img_fail(d, "bad APNG frame"), false;
    if (c[24] > 2 || c[25] > 1)
        return au_img_fail(d, "bad APNG frame"), false;
    if (!p->seen_idat && (x || y || w != (uint32_t)p->w || h != (uint32_t)p->h))
        return au_img_fail(d, "bad APNG first frame"), false;
    if (p->have_fctl)
        return au_img_fail(d, "APNG frame without data"), false;
    p->fx = (int)x;
    p->fy = (int)y;
    p->fw = (int)w;
    p->fh = (int)h;
    if (!den)
        den = 100;
    p->delay = (uint32_t)(((uint64_t)num * 1000 + den / 2) / den);
    p->dispose = c[24];
    p->blend = c[25];
    p->have_fctl = true;
    if (!p->seen_idat)
        p->idat_is_frame = true;
    return true;
}

/* Disposes of the previous frame before the next one is drawn. */
static void dispose_prev(Png *p, AuImageDecoder *d)
{
    if (!p->prev_valid)
        return;
    if (p->pdispose == 1)
        au_img_clear_rect(d, p->px, p->py, p->pw, p->ph);
    else if (p->pdispose == 2) {
        au_img_restore_rect(d, p->saved, p->px, p->py, p->pw, p->ph);
        p->saved = NULL;
    }
    p->prev_valid = false;
}

static bool begin_canvas(Png *p, AuImageDecoder *d)
{
    if (p->begun)
        return true;
    if (!au_img_begin(d, p->w, p->h, au_img_scale(d)))
        return false;
    p->begun = true;
    return true;
}

/* An image data chunk starts: IDAT or fdAT. A chunk that continues the
 * open zlib stream (more rows, or only its trailer) carries on. */
static bool begin_data(Png *p, AuImageDecoder *d, bool fdat)
{
    if (p->stream_open)
        return true;
    if (p->ctype_px == 3 && !p->npal)
        return au_img_fail(d, "missing palette"), false;
    if (!begin_canvas(p, d))
        return false;
    if (!fdat) {
        p->hidden = p->apng && !p->idat_is_frame;
        if (!p->apng || p->hidden) {
            p->fx = p->fy = 0;
            p->fw = p->w;
            p->fh = p->h;
            p->blend = 0;
            p->dispose = 0;
        }
        if (p->apng && p->idat_is_frame) {
            p->have_fctl = false;
            if (p->dispose == 2)
                p->dispose = 1; /* nothing to go back to */
        }
    } else {
        if (!p->have_fctl)
            return au_img_fail(d, "APNG data without a frame"), false;
        p->have_fctl = false;
        p->hidden = false;
        if (p->default_drawn) {
            /* The default image was shown while it loaded; the animation
             * starts from a transparent canvas. */
            au_img_clear_rect(d, 0, 0, p->w, p->h);
            p->default_drawn = false;
        }
        dispose_prev(p, d);
        if (p->dispose == 2)
            p->saved = au_img_save_rect(d, p->fx, p->fy, p->fw, p->fh);
    }
    p->stream_open = true;
    return start_frame_rows(p, d);
}

static AuImageStatus finish_frame(Png *p, AuImageDecoder *d)
{
    p->frame_pending = false;
    p->in_frame = false;
    if (p->hidden) {
        /* Kept in the canvas in case the animation has no frames. */
        p->hidden = false;
        p->default_drawn = true;
        return AU_IMAGE_MORE;
    }
    p->frames_done++;
    if (p->apng) {
        p->prev_valid = true;
        p->px = p->fx;
        p->py = p->fy;
        p->pw = p->fw;
        p->ph = p->fh;
        p->pdispose = p->dispose;
    }
    au_img_frame_done(d, p->apng ? p->delay : 0);
    return AU_IMAGE_FRAME;
}

/* A held chunk is complete and its CRC good. */
static bool use_chunk(Png *p, AuImageDecoder *d, const uint8_t *c, uint32_t n)
{
    switch (p->ctype) {
    case T_IHDR:
        if (p->have_ihdr || n != 13)
            return au_img_fail(d, "bad IHDR"), false;
        return parse_ihdr(p, d, c);
    case T_PLTE:
        if (p->seen_plte || p->seen_idat || n % 3 || n == 0 || n > 768)
            return p->ctype_px == 3 ? (au_img_fail(d, "bad PLTE"), false) : true;
        p->seen_plte = true;
        if (p->ctype_px == 0 || p->ctype_px == 4)
            return true; /* not allowed; ignored */
        p->npal = (int)(n / 3);
        for (int i = 0; i < p->npal; i++)
            p->pal[i] = 0xFF000000u | (uint32_t)c[3 * i] << 16 | (uint32_t)c[3 * i + 1] << 8 | c[3 * i + 2];
        return true;
    case T_tRNS:
        if (p->seen_idat || p->have_trns)
            return true;
        if (p->ctype_px == 3) {
            for (uint32_t i = 0; i < n && i < 256; i++)
                p->pal[i] = (p->pal[i] & 0x00FFFFFFu) | (uint32_t)c[i] << 24;
            /* Entries past PLTE's count keep opaque black when used. */
            p->have_trns = true;
        } else if (p->ctype_px == 0 && n == 2) {
            p->trns_key[0] = (uint16_t)au_img_be16(c);
            p->have_trns = true;
        } else if (p->ctype_px == 2 && n == 6) {
            p->trns_key[0] = (uint16_t)au_img_be16(c);
            p->trns_key[1] = (uint16_t)au_img_be16(c + 2);
            p->trns_key[2] = (uint16_t)au_img_be16(c + 4);
            p->have_trns = true;
        }
        if (p->have_trns)
            d->info.has_alpha = true;
        return true;
    case T_acTL:
        if (n != 8 || p->seen_idat || p->apng)
            return true;
        p->nframes_decl = au_img_be32(c);
        if (p->nframes_decl == 0 || p->nframes_decl > MAX_FRAMES)
            return true; /* not a valid animation: the still image */
        p->apng = true;
        au_img_set_animation(d, p->nframes_decl > 1, (int)(au_img_be32(c + 4) > 0x7FFFFFFF ? 0 : au_img_be32(c + 4)));
        return true;
    case T_fcTL:
        if (!p->apng || n != 26)
            return true;
        return parse_fctl(p, d, c);
    default:
        return true;
    }
}

static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    Png *p = st;
    (void)final;
    for (;;) {
        /* A frame is reported once the CRC of the chunk holding its last
         * row has been checked. */
        if (p->frame_pending && p->state != S_DATA && p->state != S_SKIP && p->state != S_CRC &&
            finish_frame(p, d) == AU_IMAGE_FRAME)
            return AU_IMAGE_FRAME;
        if (p->state == S_END)
            return AU_IMAGE_DONE;
        if (!*len)
            return AU_IMAGE_MORE;
        switch (p->state) {
        case S_SIG: {
            if (!au_img_hold(&p->hold, 8, in, len))
                return AU_IMAGE_MORE;
            if (memcmp(p->hold.buf, "\x89PNG\r\n\x1a\n", 8))
                return au_img_fail(d, "not a PNG");
            p->hold.len = 0;
            p->state = S_HDR;
            break;
        }
        case S_HDR: {
            size_t n = 8 - p->nhdr;
            if (n > *len)
                n = *len;
            memcpy(p->hdr + p->nhdr, *in, n);
            p->nhdr += n;
            *in += n;
            *len -= n;
            if (p->nhdr < 8)
                return AU_IMAGE_MORE;
            p->nhdr = 0;
            p->clen = au_img_be32(p->hdr);
            p->ctype = au_img_be32(p->hdr + 4);
            if (p->clen > 0x7FFFFFFFu)
                return au_img_fail(d, "bad chunk length");
            for (int i = 4; i < 8; i++) {
                uint8_t ch = p->hdr[i];
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
                    return au_img_fail(d, "bad chunk type");
            }
            p->cleft = p->clen;
            p->crc = au_codec_crc32(0, p->hdr + 4, 4);
            if (!p->have_ihdr && p->ctype != T_IHDR)
                return au_img_fail(d, "missing IHDR");
            p->held = false;
            bool data = p->ctype == T_IDAT || (p->ctype == T_fdAT && p->apng && p->idat_done);
            if (!data || (p->ctype == T_IDAT) != p->stream_is_idat) {
                /* A frame's data chunks end at any other chunk. */
                if (p->in_frame)
                    return au_img_fail(d, "not enough image data");
                p->stream_open = false;
            }
            if (p->ctype == T_IDAT) {
                if (p->idat_done || (p->seen_idat && !p->stream_open)) {
                    p->state = S_SKIP; /* IDAT after the image: ignored */
                    break;
                }
                p->seen_idat = true;
                p->stream_is_idat = true;
                if (!begin_data(p, d, false))
                    return AU_IMAGE_ERROR;
                p->state = S_DATA;
            } else if (data) {
                if (p->clen < 4)
                    return au_img_fail(d, "bad fdAT");
                p->stream_is_idat = false;
                p->state = S_SEQ;
                p->hold.len = 0;
            } else if (p->ctype == T_IHDR || p->ctype == T_PLTE || p->ctype == T_tRNS || p->ctype == T_acTL ||
                       p->ctype == T_fcTL) {
                if (p->clen > MAX_HOLD)
                    return critical(p->ctype) ? au_img_fail(d, "chunk too long") : (p->state = S_SKIP, AU_IMAGE_MORE);
                p->hold.len = 0;
                p->state = S_HOLD;
            } else if (p->ctype == T_IEND || p->ctype == T_fdAT) {
                p->state = S_SKIP;
            } else {
                if (critical(p->ctype))
                    return au_img_fail(d, "unknown critical chunk");
                p->state = S_SKIP;
            }
            if (p->state != S_DATA && p->stream_is_idat && p->seen_idat && !p->stream_open)
                p->idat_done = true;
            if (p->cleft == 0 && p->state != S_HOLD && p->state != S_SEQ)
                p->state = S_CRC;
            break;
        }
        case S_HOLD:
            if (!au_img_hold(&p->hold, p->clen, in, len))
                return AU_IMAGE_MORE;
            p->crc = au_codec_crc32(p->crc, p->hold.buf, p->clen);
            p->held = true;
            p->cleft = 0;
            p->state = S_CRC;
            break;
        case S_SEQ: {
            if (!au_img_hold(&p->hold, 4, in, len))
                return AU_IMAGE_MORE;
            p->crc = au_codec_crc32(p->crc, p->hold.buf, 4);
            p->cleft -= 4;
            if (au_img_be32(p->hold.buf) != p->next_seq)
                return au_img_fail(d, "APNG sequence error");
            p->next_seq++;
            p->hold.len = 0;
            if (!p->stream_open && !p->have_fctl) {
                p->state = p->cleft ? S_SKIP : S_CRC; /* data after its frame's stream ended */
                break;
            }
            if (!begin_data(p, d, true))
                return AU_IMAGE_ERROR;
            p->state = p->cleft ? S_DATA : S_CRC;
            break;
        }
        case S_DATA: {
            size_t n = p->cleft < *len ? p->cleft : *len, used = 0;
            if (!p->stream_open) {
                p->state = S_SKIP;
                break;
            }
            if (!feed_data(p, d, *in, n, &used))
                return AU_IMAGE_ERROR;
            p->crc = au_codec_crc32(p->crc, *in, used);
            *in += used;
            *len -= used;
            p->cleft -= (uint32_t)used;
            if (!p->cleft)
                p->state = S_CRC;
            else if (!p->stream_open)
                p->state = S_SKIP; /* data after the end of the stream */
            break;
        }
        case S_SKIP: {
            size_t n = p->cleft < *len ? p->cleft : *len;
            p->crc = au_codec_crc32(p->crc, *in, n);
            *in += n;
            *len -= n;
            p->cleft -= (uint32_t)n;
            if (!p->cleft)
                p->state = S_CRC;
            break;
        }
        case S_CRC: {
            size_t n = 4 - p->ncrc;
            if (n > *len)
                n = *len;
            memcpy(p->crcbuf + p->ncrc, *in, n);
            p->ncrc += n;
            *in += n;
            *len -= n;
            if (p->ncrc < 4)
                return AU_IMAGE_MORE;
            p->ncrc = 0;
            bool good = au_img_be32(p->crcbuf) == p->crc;
            if (!good && critical(p->ctype))
                return au_img_fail(d, "CRC error");
            if (good && p->held && !use_chunk(p, d, p->hold.buf, p->clen))
                return AU_IMAGE_ERROR;
            p->held = false;
            if (p->hold.cap > 4096)
                au_img_hold_free(&p->hold);
            p->hold.len = 0;
            p->state = S_HDR;
            if (p->ctype == T_IEND) {
                if (p->in_frame)
                    return au_img_fail(d, "not enough image data");
                p->state = S_END;
                if (p->frames_done == 0 && p->default_drawn) {
                    /* An APNG whose animation has no frames: show the
                     * default image. */
                    au_img_frame_done(d, 0);
                    return AU_IMAGE_FRAME;
                }
                if (p->frames_done == 0)
                    return au_img_fail(d, "no image data");
                return AU_IMAGE_DONE;
            }
            break;
        }
        default:
            break;
        }
    }
}

const AuImgCodec au_img_png_codec = {create, run, destroy};
