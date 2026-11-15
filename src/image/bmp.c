/* BMP and ICO, and favicon decoding.
 *
 * BMP: the Windows (BITMAPINFOHEADER, V2 to V5) and OS/2 (12, 16 and 64
 * byte) headers; 1, 4 and 8 bits with a palette, 16 and 32 bits with
 * default or explicit bit fields (alpha from an alpha mask only: the
 * fourth byte of a plain 32-bit pixel is padding, as in browsers), 24
 * bits; RLE8 and RLE4 (pixels a run skips are transparent); top-down
 * and bottom-up rows. Rows stream: each row is converted as its bytes
 * arrive (bottom-up files fill in from the bottom). Embedded JPEG or PNG
 * (compression 4 and 5) and OS/2 Huffman and RLE24 are refused.
 *
 * ICO (and CUR): the directory is read, the best entry chosen (the
 * smallest at least as large as the target size, else the largest; the
 * deepest colour among equal sizes), and that entry is buffered and
 * decoded when it is complete: PNG entries through the PNG decoder, BMP
 * entries (a header without a file header, twice the height, an XOR
 * image and a 1-bit AND mask) here, the AND mask giving transparency
 * unless a 32-bit entry has alpha of its own. Entries are limited to
 * 4 MB. */
#include "image_int.h"

enum { B_FILEHDR, B_INFO, B_EXTRA, B_GAP, B_ROWS, B_RLE, B_ANDMASK, B_DONE };

typedef struct {
    int state;
    AuImgHold hold;
    bool ico;         /* an ICO's BMP entry: no file header, AND mask, output kept in img */
    uint32_t pos;     /* bytes consumed so far */
    uint32_t off;     /* pixel data offset (from the start of the stream) */
    uint32_t hs;      /* info header size */
    int w, h;         /* image size (h without the ICO doubling) */
    bool top_down;
    int bpp, comp;
    uint32_t mask[4]; /* R, G, B, A */
    int shift[4], bits[4];
    uint32_t pal[256];
    int npal, pal_entry;
    size_t stride;
    int row;          /* rows done (file order) */
    uint32_t *line;   /* w pixels */
    /* RLE. */
    int rx, ry;       /* position (file order rows) */
    int rle_state;
    int abs_n;
    /* ICO output. */
    uint32_t *img;    /* w * h non-premultiplied pixels, top-down */
    bool any_alpha;
} Bmp;

static void *bmp_create(AuImageDecoder *d)
{
    Bmp *b = au_calloc(sizeof *b);
    b->state = B_FILEHDR;
    au_img_mem(d, (ptrdiff_t)sizeof *b);
    return b;
}

static void bmp_destroy(void *st)
{
    Bmp *b = st;
    au_img_hold_free(&b->hold);
    au_free(b->line);
    au_free(b->img);
    au_free(b);
}

static void set_mask(Bmp *b, int i, uint32_t m)
{
    b->mask[i] = m;
    b->shift[i] = 0;
    b->bits[i] = 0;
    if (!m)
        return;
    while (!(m & 1)) {
        m >>= 1;
        b->shift[i]++;
    }
    while (m & 1) {
        m >>= 1;
        b->bits[i]++;
    }
}

static inline uint32_t chan(const Bmp *b, int i, uint32_t v)
{
    if (!b->mask[i])
        return i == 3 ? 255 : 0;
    uint32_t x = (v & b->mask[i]) >> b->shift[i];
    int n = b->bits[i];
    if (n >= 8)
        return (x >> (n - 8)) & 255;
    /* Fewer bits: replicate them into the low bits, as browsers do. */
    uint32_t r = 0;
    for (int got = 0; got < 8; got += n)
        r = (r << n) | x;
    return (r >> (((8 + n - 1) / n) * n - 8)) & 255;
}

/* Parses the info header in buf (hs bytes) and checks it. */
static bool parse_info(Bmp *b, AuImageDecoder *d, const uint8_t *p)
{
    uint32_t hs = b->hs;
    int32_t w, h;
    int planes;
    b->comp = 0;
    if (hs == 12) {
        w = (int32_t)au_img_le16(p + 4);
        h = (int32_t)au_img_le16(p + 6);
        planes = (int)au_img_le16(p + 8);
        b->bpp = (int)au_img_le16(p + 10);
        b->pal_entry = 3;
    } else {
        w = (int32_t)au_img_le32(p + 4);
        h = (int32_t)au_img_le32(p + 8);
        planes = (int)au_img_le16(p + 12);
        b->bpp = (int)au_img_le16(p + 14);
        if (hs >= 20)
            b->comp = (int)au_img_le32(p + 16);
        b->pal_entry = 4;
    }
    (void)planes;
    if (b->ico) {
        if (h < 0)
            return au_img_fail(d, "bad icon"), false;
        h /= 2;
    }
    if (h == INT32_MIN || w <= 0 || h == 0)
        return au_img_fail(d, "bad BMP size"), false;
    b->top_down = h < 0;
    if (h < 0)
        h = -h;
    if (w > AU_IMAGE_MAX_DIM || h > AU_IMAGE_MAX_DIM)
        return au_img_fail(d, "image too large"), false;
    b->w = (int)w;
    b->h = (int)h;
    bool os2v2 = hs == 16 || hs == 64;
    switch (b->comp) {
    case 0:
        if (b->bpp != 1 && b->bpp != 4 && b->bpp != 8 && b->bpp != 16 && b->bpp != 24 && b->bpp != 32)
            return au_img_fail(d, "bad BMP depth"), false;
        break;
    case 1:
        if (b->bpp != 8 || b->top_down)
            return au_img_fail(d, "bad RLE8 BMP"), false;
        break;
    case 2:
        if (b->bpp != 4 || b->top_down)
            return au_img_fail(d, "bad RLE4 BMP"), false;
        break;
    case 3:
    case 6:
        if (os2v2)
            return au_img_fail(d, "unsupported OS/2 BMP compression"), false;
        if (b->bpp != 16 && b->bpp != 32)
            return au_img_fail(d, "bad bit fields BMP"), false;
        break;
    default:
        return au_img_fail(d, "unsupported BMP compression"), false;
    }
    if (b->bpp == 16) {
        set_mask(b, 0, 0x7C00);
        set_mask(b, 1, 0x03E0);
        set_mask(b, 2, 0x001F);
    } else if (b->bpp == 32) {
        set_mask(b, 0, 0xFF0000);
        set_mask(b, 1, 0xFF00);
        set_mask(b, 2, 0xFF);
    }
    set_mask(b, 3, 0);
    if ((b->comp == 3 || b->comp == 6) && hs >= 52) {
        set_mask(b, 0, au_img_le32(p + 40));
        set_mask(b, 1, au_img_le32(p + 44));
        set_mask(b, 2, au_img_le32(p + 48));
    }
    if (hs >= 56 && (b->comp == 3 || b->comp == 6))
        set_mask(b, 3, au_img_le32(p + 52));
    if (b->ico && b->bpp == 32 && b->comp == 0)
        set_mask(b, 3, 0xFF000000u); /* 32-bit icons carry alpha */
    uint32_t used = hs >= 36 && hs != 12 ? au_img_le32(p + 32) : 0;
    if (b->bpp <= 8)
        b->npal = used && used < 256 ? (int)used : 1 << b->bpp;
    else
        b->npal = 0;
    size_t sb;
    if (!au_img_mul((size_t)b->w, (size_t)b->bpp, &sb))
        return au_img_fail(d, "bad BMP size"), false;
    b->stride = (sb + 31) / 32 * 4;
    return true;
}

/* Bytes of masks and palette after the info header. */
static uint32_t extra_size(const Bmp *b)
{
    uint32_t m = 0;
    if (b->hs == 40 && b->comp == 3)
        m = 12;
    else if (b->hs == 40 && b->comp == 6)
        m = 16;
    return m + (uint32_t)b->npal * (uint32_t)b->pal_entry;
}

static void parse_extra(Bmp *b, const uint8_t *p, uint32_t n)
{
    uint32_t m = 0;
    if (b->hs == 40 && (b->comp == 3 || b->comp == 6) && n >= 12) {
        set_mask(b, 0, au_img_le32(p));
        set_mask(b, 1, au_img_le32(p + 4));
        set_mask(b, 2, au_img_le32(p + 8));
        m = 12;
        if (b->comp == 6 && n >= 16) {
            set_mask(b, 3, au_img_le32(p + 12));
            m = 16;
        }
    }
    int avail = (int)((n - m) / (uint32_t)b->pal_entry);
    if (avail < b->npal)
        b->npal = avail;
    for (int i = 0; i < b->npal; i++) {
        const uint8_t *e = p + m + (size_t)i * (size_t)b->pal_entry;
        b->pal[i] = 0xFF000000u | (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0];
    }
}

static bool begin_output(Bmp *b, AuImageDecoder *d)
{
    au_img_mem(d, (ptrdiff_t)((size_t)b->w * 4));
    b->line = au_alloc((size_t)b->w * 4);
    if (b->ico) {
        size_t px, bytes;
        if (!au_img_mul((size_t)b->w, (size_t)b->h, &px) || !au_img_mul(px, 4, &bytes) || bytes > d->opt.max_bytes)
            return au_img_fail(d, "icon too large"), false;
        b->img = au_calloc(bytes);
        au_img_mem(d, (ptrdiff_t)bytes);
        return true;
    }
    bool alpha = b->mask[3] != 0;
    if (!au_img_header(d, b->w, b->h, 1, alpha || b->comp == 1 || b->comp == 2))
        return false;
    return au_img_begin(d, b->w, b->h, au_img_scale(d));
}

/* A finished row in file order r. */
static void put_line(Bmp *b, AuImageDecoder *d, int r)
{
    int y = b->top_down ? r : b->h - 1 - r;
    if (y < 0 || y >= b->h)
        return;
    if (b->ico) {
        memcpy(b->img + (size_t)y * (size_t)b->w, b->line, (size_t)b->w * 4);
        return;
    }
    if (b->top_down)
        au_img_row(d, y, b->line);
    else
        au_img_row_at(d, y, 1, b->line);
}

static void convert_row(Bmp *b, const uint8_t *p)
{
    uint32_t *o = b->line;
    int w = b->w;
    switch (b->bpp) {
    case 1:
    case 4:
    case 8: {
        int per = 8 / b->bpp, m = (1 << b->bpp) - 1;
        for (int x = 0; x < w; x++) {
            int i = b->bpp == 8 ? p[x] : (p[x / per] >> ((per - 1 - x % per) * b->bpp)) & m;
            o[x] = i < b->npal ? b->pal[i] : 0xFF000000u;
        }
        break;
    }
    case 24:
        for (int x = 0; x < w; x++)
            o[x] = 0xFF000000u | (uint32_t)p[3 * x + 2] << 16 | (uint32_t)p[3 * x + 1] << 8 | p[3 * x];
        break;
    default: { /* 16, 32: bit fields */
        for (int x = 0; x < w; x++) {
            uint32_t v = b->bpp == 16 ? au_img_le16(p + 2 * x) : au_img_le32(p + 4 * x);
            uint32_t a = chan(b, 3, v);
            if (a)
                b->any_alpha = true;
            o[x] = a << 24 | chan(b, 0, v) << 16 | chan(b, 1, v) << 8 | chan(b, 2, v);
        }
        break;
    }
    }
}

/* ---- RLE ---- */

static inline void rle_px(Bmp *b, int index)
{
    if (b->rx < b->w)
        b->line[b->rx] = index < b->npal ? b->pal[index] : 0xFF000000u;
    b->rx++;
}

static void rle_newline(Bmp *b, AuImageDecoder *d)
{
    if (b->ry < b->h)
        put_line(b, d, b->ry);
    b->ry++;
    b->rx = 0;
    memset(b->line, 0, (size_t)b->w * 4);
}

/* Runs RLE commands; true when the bitmap's end is reached. */
static bool run_rle(Bmp *b, AuImageDecoder *d, const uint8_t **in, size_t *len)
{
    while (*len && b->ry < b->h) {
        if (b->rle_state == 0) {
            if (!au_img_hold(&b->hold, 2, in, len))
                return false;
            uint8_t n = b->hold.buf[0], c = b->hold.buf[1];
            b->hold.len = 0;
            if (n) {
                for (int i = 0; i < n; i++)
                    rle_px(b, b->comp == 1 ? c : (i & 1) ? (c & 15) : (c >> 4));
            } else if (c == 0) {
                rle_newline(b, d);
            } else if (c == 1) {
                while (b->ry < b->h)
                    rle_newline(b, d);
                return true;
            } else if (c == 2) {
                b->rle_state = 1;
            } else {
                b->abs_n = c;
                b->rle_state = 2;
            }
        } else if (b->rle_state == 1) {
            if (!au_img_hold(&b->hold, 2, in, len))
                return false;
            int dx = b->hold.buf[0], dy = b->hold.buf[1];
            b->hold.len = 0;
            for (int k = 0; k < dy && b->ry < b->h; k++) {
                int x = b->rx;
                rle_newline(b, d);
                b->rx = x;
            }
            b->rx += dx;
            b->rle_state = 0;
        } else {
            int n = b->abs_n;
            size_t bytes = b->comp == 1 ? (size_t)n : ((size_t)n + 1) / 2;
            bytes = (bytes + 1) & ~(size_t)1;
            if (!au_img_hold(&b->hold, bytes, in, len))
                return false;
            for (int i = 0; i < n; i++) {
                const uint8_t *q = b->hold.buf;
                rle_px(b, b->comp == 1 ? q[i] : (i & 1) ? (q[i / 2] & 15) : (q[i / 2] >> 4));
            }
            b->hold.len = 0;
            b->rle_state = 0;
        }
    }
    return b->ry >= b->h;
}

/* Skips n bytes of input, counting them. */
static size_t skip(Bmp *b, const uint8_t **in, size_t *len, size_t n)
{
    if (n > *len)
        n = *len;
    *in += n;
    *len -= n;
    b->pos += (uint32_t)n;
    return n;
}

static AuImageStatus finish(Bmp *b, AuImageDecoder *d)
{
    b->state = B_DONE;
    if (b->ico)
        return AU_IMAGE_DONE;
    au_img_frame_done(d, 0);
    return AU_IMAGE_FRAME;
}

static AuImageStatus bmp_run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    Bmp *b = st;
    (void)final;
    for (;;) {
        if (b->state == B_DONE)
            return AU_IMAGE_DONE;
        if (!*len)
            return AU_IMAGE_MORE;
        size_t before = *len;
        int before_state = b->state;
        switch (b->state) {
        case B_FILEHDR:
            if (b->ico) {
                b->state = B_INFO;
                break;
            }
            if (!au_img_hold(&b->hold, 18, in, len))
                break;
            if (b->hold.buf[0] != 'B' || b->hold.buf[1] != 'M')
                return au_img_fail(d, "not a BMP");
            b->off = au_img_le32(b->hold.buf + 10);
            b->hs = au_img_le32(b->hold.buf + 14);
            if (b->hs != 12 && b->hs != 16 && b->hs != 40 && b->hs != 52 && b->hs != 56 && b->hs != 64 &&
                b->hs != 108 && b->hs != 124)
                return au_img_fail(d, "bad BMP header");
            b->state = B_INFO;
            break;
        case B_INFO: {
            if (b->ico && b->hold.len == 0 && b->hs == 0) {
                if (!au_img_hold(&b->hold, 4, in, len))
                    break;
                b->hs = au_img_le32(b->hold.buf);
                if (b->hs != 12 && b->hs != 40 && b->hs != 52 && b->hs != 56 && b->hs != 108 && b->hs != 124)
                    return au_img_fail(d, "bad icon header");
            }
            size_t base = b->ico ? 0 : 14;
            if (!au_img_hold(&b->hold, base + b->hs, in, len))
                break;
            if (!parse_info(b, d, b->hold.buf + base))
                return AU_IMAGE_ERROR;
            uint32_t hdr_end = (uint32_t)base + b->hs;
            uint32_t ex = extra_size(b);
            if (b->ico)
                b->off = hdr_end + ex;
            if (b->off < hdr_end)
                return au_img_fail(d, "bad BMP offset");
            /* Palette entries beyond the pixel offset are not there. */
            if (ex > b->off - hdr_end)
                ex = b->off - hdr_end;
            if (ex > 16 + 256 * 4)
                ex = 16 + 256 * 4;
            b->pos = hdr_end;
            b->hold.len = 0;
            b->abs_n = (int)ex; /* reused: bytes of extra to hold */
            b->state = B_EXTRA;
            if (ex == 0) {
                parse_extra(b, NULL, 0);
                if (!begin_output(b, d))
                    return AU_IMAGE_ERROR;
                b->state = B_GAP;
            }
            break;
        }
        case B_EXTRA:
            if (!au_img_hold(&b->hold, (size_t)b->abs_n, in, len))
                break;
            parse_extra(b, b->hold.buf, (uint32_t)b->abs_n);
            b->pos += (uint32_t)b->abs_n;
            b->hold.len = 0;
            b->abs_n = 0;
            if (!begin_output(b, d))
                return AU_IMAGE_ERROR;
            b->state = B_GAP;
            break;
        case B_GAP:
            if (b->pos < b->off) {
                skip(b, in, len, b->off - b->pos);
                break;
            }
            b->state = b->comp == 1 || b->comp == 2 ? B_RLE : B_ROWS;
            if (b->state == B_RLE)
                memset(b->line, 0, (size_t)b->w * 4);
            break;
        case B_ROWS:
            if (!au_img_hold(&b->hold, b->stride, in, len))
                break;
            convert_row(b, b->hold.buf);
            b->hold.len = 0;
            put_line(b, d, b->row);
            if (++b->row >= b->h) {
                if (b->ico) {
                    b->row = 0;
                    b->state = B_ANDMASK;
                    break;
                }
                return finish(b, d);
            }
            break;
        case B_RLE:
            if (run_rle(b, d, in, len))
                return finish(b, d);
            break;
        case B_ANDMASK: {
            size_t ms = ((size_t)b->w + 31) / 32 * 4;
            if (!au_img_hold(&b->hold, ms, in, len))
                break;
            int y = b->top_down ? b->row : b->h - 1 - b->row;
            uint32_t *row = b->img + (size_t)y * (size_t)b->w;
            bool use_mask = b->bpp < 32 || !b->any_alpha;
            for (int x = 0; x < b->w && use_mask; x++) {
                bool clear = (b->hold.buf[x / 8] >> (7 - x % 8)) & 1;
                row[x] = clear ? 0 : (row[x] | 0xFF000000u);
            }
            b->hold.len = 0;
            if (++b->row >= b->h)
                return finish(b, d);
            break;
        }
        default:
            return au_img_fail(d, "bad state");
        }
        if (*len == before && *len && b->state == before_state)
            return au_img_fail(d, "stalled");
        if (!*len && b->state != B_DONE)
            return AU_IMAGE_MORE;
    }
}

const AuImgCodec au_img_bmp_codec = {bmp_create, bmp_run, bmp_destroy};

/* ---- ICO ---- */

#define ICO_MAX_ENTRY ((uint32_t)4 * 1024 * 1024)

enum { I_HDR, I_DIR, I_SKIP, I_ENTRY, I_DONE };

typedef struct {
    int state;
    AuImgHold hold;
    int count;
    uint32_t pos;
    uint32_t eoff, esize;
    int ew, eh;
} Ico;

static void *ico_create(AuImageDecoder *d)
{
    (void)d;
    Ico *c = au_calloc(sizeof *c);
    return c;
}

static void ico_destroy(void *st)
{
    Ico *c = st;
    au_img_hold_free(&c->hold);
    au_free(c);
}

/* Picks the entry: the smallest at least the target size (the largest
 * side of the target), else the largest; deeper colour breaks ties. */
static int choose_entry(const uint8_t *dir, int n, int target)
{
    int best = -1, bs = 0, bb = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = dir + 16 * i;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, bpp = (int)au_img_le16(e + 6);
        int s = w > h ? w : h;
        if (best < 0) {
            best = i, bs = s, bb = bpp;
            continue;
        }
        bool better;
        if (target > 0) {
            bool fits = s >= target, bfits = bs >= target;
            if (fits != bfits)
                better = fits;
            else if (s != bs)
                better = fits ? s < bs : s > bs;
            else
                better = bpp > bb;
        } else {
            better = s > bs || (s == bs && bpp > bb);
        }
        if (better)
            best = i, bs = s, bb = bpp;
    }
    return best;
}

/* Decodes the buffered entry into d's canvas. */
static AuImageStatus decode_entry(Ico *c, AuImageDecoder *d, const uint8_t *p, size_t n)
{
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        AuImageOptions o = d->opt;
        o.first_frame_only = true;
        o.header_only = false;
        AuImageDecoder *in = au_image_decoder_new(&o);
        const uint8_t *q = p;
        size_t qn = n;
        AuImageStatus st = au_image_decode(in, &q, &qn, true);
        const AuImageInfo *ii = au_image_info(in);
        if ((st != AU_IMAGE_FRAME && !ii->partial) || !ii->ready) {
            au_img_fail(d, "bad PNG icon entry");
            au_image_decoder_free(in);
            return AU_IMAGE_ERROR;
        }
        /* Take the inner canvas as ours. */
        if (!au_img_header(d, ii->width, ii->height, 1, ii->has_alpha)) {
            au_image_decoder_free(in);
            return AU_IMAGE_ERROR;
        }
        d->canvas = au_image_take_canvas(in);
        d->cw = ii->width;
        d->ch = ii->height;
        d->red = ii->scale_denom;
        d->info.out_w = d->canvas->width;
        d->info.out_h = d->canvas->height;
        d->info.scale_denom = ii->scale_denom;
        d->next_row = d->ch;
        d->any_dirty = true;
        d->dirty = (AuIRect){0, 0, d->canvas->width, d->canvas->height};
        au_image_decoder_free(in);
        au_img_frame_done(d, 0);
        c->state = I_DONE;
        return AU_IMAGE_FRAME;
    }
    /* A BMP entry, through the BMP decoder in icon mode. */
    Bmp *b = bmp_create(d);
    b->ico = true;
    const uint8_t *q = p;
    size_t qn = n;
    AuImageStatus st = bmp_run(b, d, &q, &qn, true);
    if (d->failed || !b->img) {
        bmp_destroy(b);
        return au_img_fail(d, "bad icon entry");
    }
    (void)st;
    /* A truncated entry keeps what decoded (rows not reached are
     * transparent). */
    if (!au_img_header(d, b->w, b->h, 1, true) || !au_img_begin(d, b->w, b->h, au_img_scale(d))) {
        bmp_destroy(b);
        return AU_IMAGE_ERROR;
    }
    for (int y = 0; y < b->h; y++)
        au_img_row(d, y, b->img + (size_t)y * (size_t)b->w);
    au_img_mem(d, -(ptrdiff_t)((size_t)b->w * (size_t)b->h * 4 + (size_t)b->w * 4 + sizeof *b));
    bmp_destroy(b);
    au_img_frame_done(d, 0);
    c->state = I_DONE;
    return AU_IMAGE_FRAME;
}

static AuImageStatus ico_run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    Ico *c = st;
    for (;;) {
        if (c->state == I_DONE)
            return AU_IMAGE_DONE;
        switch (c->state) {
        case I_HDR:
            if (!au_img_hold(&c->hold, 6, in, len))
                return AU_IMAGE_MORE;
            c->count = (int)au_img_le16(c->hold.buf + 4);
            if (c->hold.buf[0] || c->hold.buf[1] || (c->hold.buf[2] != 1 && c->hold.buf[2] != 2) || c->count < 1 ||
                c->count > 255)
                return au_img_fail(d, "not an icon");
            c->hold.len = 0;
            c->state = I_DIR;
            break;
        case I_DIR: {
            if (!au_img_hold(&c->hold, (size_t)c->count * 16, in, len))
                return AU_IMAGE_MORE;
            int t = d->opt.target_w > d->opt.target_h ? d->opt.target_w : d->opt.target_h;
            int i = choose_entry(c->hold.buf, c->count, t);
            const uint8_t *e = c->hold.buf + 16 * i;
            c->ew = e[0] ? e[0] : 256;
            c->eh = e[1] ? e[1] : 256;
            c->esize = au_img_le32(e + 8);
            c->eoff = au_img_le32(e + 12);
            c->pos = 6 + (uint32_t)c->count * 16;
            if (c->esize < 8 || c->esize > ICO_MAX_ENTRY || c->eoff < c->pos)
                return au_img_fail(d, "bad icon entry");
            /* The directory's size, until the entry says otherwise. */
            if (!au_img_header(d, c->ew, c->eh, 1, true))
                return AU_IMAGE_ERROR;
            if (d->opt.header_only) {
                c->state = I_DONE; /* the directory's size is the header's */
                return AU_IMAGE_DONE;
            }
            c->hold.len = 0;
            c->state = I_SKIP;
            break;
        }
        case I_SKIP: {
            size_t n = c->eoff - c->pos;
            if (n > *len)
                n = *len;
            *in += n;
            *len -= n;
            c->pos += (uint32_t)n;
            if (c->pos < c->eoff)
                return final ? au_img_fail(d, "truncated") : AU_IMAGE_MORE;
            c->state = I_ENTRY;
            break;
        }
        case I_ENTRY:
            if (!au_img_hold(&c->hold, c->esize, in, len)) {
                if (final && c->hold.len >= 40)
                    return decode_entry(c, d, c->hold.buf, c->hold.len);
                return AU_IMAGE_MORE;
            }
            au_img_mem(d, (ptrdiff_t)c->esize);
            {
                AuImageStatus s = decode_entry(c, d, c->hold.buf, c->esize);
                au_img_mem(d, -(ptrdiff_t)c->esize);
                au_img_hold_free(&c->hold);
                return s;
            }
        default:
            return au_img_fail(d, "bad state");
        }
    }
}

const AuImgCodec au_img_ico_codec = {ico_create, ico_run, ico_destroy};

/* ---- Favicons ---- */

AuSurface *au_image_decode_favicon(const void *data, size_t len, int size)
{
    if (size < 1 || size > 1024)
        return NULL;
    AuImageOptions o = {0};
    o.target_w = o.target_h = size;
    o.max_bytes = (size_t)4 * 1024 * 1024;
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
    if (!s)
        return NULL;
    /* Fit inside size x size, keeping the aspect ratio, centred. */
    int w = size, h = size;
    if (s->width > s->height)
        h = (int)((int64_t)size * s->height / s->width);
    else if (s->height > s->width)
        w = (int)((int64_t)size * s->width / s->height);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    AuSurface *r = au_image_resample(s, w, h);
    au_surface_free(s);
    if (w == size && h == size)
        return r;
    AuSurface *out = au_surface_new(size, size);
    int ox = (size - w) / 2, oy = (size - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(out->pixels + (size_t)(y + oy) * out->stride + ox, r->pixels + (size_t)y * r->stride, (size_t)w * 4);
    out->opaque = false;
    au_surface_free(r);
    return out;
}
