/* The PNG encoder. See pngenc.h. */
#include "pngenc.h"

#include "../codec/codec.h"

#include <stdlib.h>
#include <string.h>

#define WIN 32768
#define HASH_BITS 12
#define HASH_SIZE (1 << HASH_BITS)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define IDAT_MAX 32768

typedef struct Enc {
    AuBuf *out;
    /* zlib output waiting to become an IDAT chunk */
    uint8_t z[IDAT_MAX + 16];
    size_t zn;
    uint32_t bits;
    int nbits;
    /* the filtered bytes: buf[0] is at stream offset base */
    uint8_t *buf;
    size_t len, cap;
    size_t pos;  /* next byte to encode */
    size_t base;
    uint32_t head[HASH_SIZE]; /* stream offset + 1 of the last string with this hash */
    uint32_t adler;
} Enc;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(AuBuf *out, const char *type, const uint8_t *data, size_t n)
{
    uint8_t h[8];
    put32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    au_buf_append(out, h, 8);
    if (n)
        au_buf_append(out, data, n);
    uint32_t crc = au_codec_crc32(0, type, 4);
    if (n)
        crc = au_codec_crc32(crc, data, n);
    put32(h, crc);
    au_buf_append(out, h, 4);
}

static void z_byte(Enc *e, uint8_t b)
{
    e->z[e->zn++] = b;
    if (e->zn >= IDAT_MAX) {
        chunk(e->out, "IDAT", e->z, e->zn);
        e->zn = 0;
    }
}

/* Bits go out least significant first. */
static void put_bits(Enc *e, uint32_t v, int n)
{
    e->bits |= v << e->nbits;
    e->nbits += n;
    while (e->nbits >= 8) {
        z_byte(e, (uint8_t)e->bits);
        e->bits >>= 8;
        e->nbits -= 8;
    }
}

/* A Huffman code goes out most significant bit first. */
static void put_code(Enc *e, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(e, r, n);
}

/* The fixed code of RFC 1951 section 3.2.6. */
static void put_symbol(Enc *e, int s)
{
    if (s < 144)
        put_code(e, 0x30 + (uint32_t)s, 8);
    else if (s < 256)
        put_code(e, 0x190 + (uint32_t)(s - 144), 9);
    else if (s < 280)
        put_code(e, (uint32_t)(s - 256), 7);
    else
        put_code(e, 0xC0 + (uint32_t)(s - 280), 8);
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(Enc *e, int len, int dist)
{
    int lc = 28;
    while (len < len_base[lc])
        lc--;
    put_symbol(e, 257 + lc);
    if (len_extra[lc])
        put_bits(e, (uint32_t)(len - len_base[lc]), len_extra[lc]);
    int dc = 29;
    while (dist < dist_base[dc])
        dc--;
    put_code(e, (uint32_t)dc, 5);
    if (dist_extra[dc])
        put_bits(e, (uint32_t)(dist - dist_base[dc]), dist_extra[dc]);
}

static inline uint32_t hash3(const uint8_t *p)
{
    return (((uint32_t)p[0] << 8) ^ ((uint32_t)p[1] << 4) ^ p[2]) & (HASH_SIZE - 1);
}

/* Encodes buffered bytes; all of them when final, else all but the last
 * MAX_MATCH (a match may need them). */
static void deflate_some(Enc *e, bool final)
{
    size_t end = final ? e->len : (e->len > MAX_MATCH ? e->len - MAX_MATCH : 0);
    while (e->pos < end) {
        const uint8_t *p = e->buf + e->pos;
        size_t avail = e->len - e->pos;
        int best = 0, dist = 0;
        if (avail >= MIN_MATCH) {
            uint32_t h = hash3(p);
            uint32_t cand = e->head[h];
            size_t here = e->base + e->pos;
            e->head[h] = (uint32_t)(here + 1);
            if (cand && cand - 1 >= e->base && here - (cand - 1) <= WIN) {
                const uint8_t *q = e->buf + (cand - 1 - e->base);
                int max = avail > MAX_MATCH ? MAX_MATCH : (int)avail;
                int n = 0;
                while (n < max && q[n] == p[n])
                    n++;
                if (n >= MIN_MATCH) {
                    best = n;
                    dist = (int)(here - (cand - 1));
                }
            }
        }
        if (best) {
            put_match(e, best, dist);
            /* Index a few of the strings the match skips. */
            for (int i = 1; i < best && i < 4; i++)
                if (e->pos + (size_t)i + MIN_MATCH <= e->len)
                    e->head[hash3(p + i)] = (uint32_t)(e->base + e->pos + (size_t)i + 1);
            e->pos += (size_t)best;
        } else {
            put_symbol(e, *p);
            e->pos++;
        }
    }
    /* Keep one window of history before pos; drop the rest. */
    if (e->pos > 2 * WIN) {
        size_t drop = e->pos - WIN;
        memmove(e->buf, e->buf + drop, e->len - drop);
        e->len -= drop;
        e->pos -= drop;
        e->base += drop;
    }
}

static void feed(Enc *e, const uint8_t *data, size_t n)
{
    e->adler = au_codec_adler32(e->adler, data, n);
    if (e->len + n > e->cap) {
        e->cap = e->len + n + 3 * WIN;
        e->buf = au_realloc(e->buf, e->cap);
    }
    memcpy(e->buf + e->len, data, n);
    e->len += n;
    deflate_some(e, false);
}

static uint32_t cost(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++)
        s += (uint32_t)abs((int8_t)p[i]);
    return s;
}

bool au_png_encode_rgba(int w, int h, AuPngRowFn row, void *ctx, AuBuf *out)
{
    if (w <= 0 || h <= 0)
        return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    au_buf_append(out, sig, 8);
    uint8_t ihdr[13] = {0};
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    chunk(out, "IHDR", ihdr, 13);

    size_t rb = (size_t)w * 4;
    Enc *e = au_calloc(sizeof *e);
    e->out = out;
    e->adler = 1;
    uint8_t *cur = au_alloc(rb), *prev = au_calloc(rb), *filt = au_alloc(3 * (rb + 1));
    z_byte(e, 0x78);
    z_byte(e, 0x01);
    put_bits(e, 1, 1); /* the last block */
    put_bits(e, 1, 2); /* fixed Huffman codes */
    for (int y = 0; y < h; y++) {
        row(ctx, y, cur);
        uint8_t *f0 = filt, *f1 = filt + rb + 1, *f2 = filt + 2 * (rb + 1);
        f0[0] = 0;
        f1[0] = 1;
        f2[0] = 2;
        memcpy(f0 + 1, cur, rb);
        for (size_t i = 0; i < rb; i++) {
            f1[1 + i] = (uint8_t)(cur[i] - (i >= 4 ? cur[i - 4] : 0));
            f2[1 + i] = (uint8_t)(cur[i] - prev[i]);
        }
        /* The filter whose bytes are smallest in sum compresses best,
         * near enough. */
        uint32_t c0 = cost(f0 + 1, rb), c1 = cost(f1 + 1, rb), c2 = cost(f2 + 1, rb);
        const uint8_t *best = c1 <= c0 && c1 <= c2 ? f1 : (c2 <= c0 ? f2 : f0);
        feed(e, best, rb + 1);
        uint8_t *t = prev;
        prev = cur;
        cur = t;
    }
    deflate_some(e, true);
    put_symbol(e, 256);
    if (e->nbits)
        put_bits(e, 0, 8 - e->nbits);
    uint8_t ad[4];
    put32(ad, e->adler);
    for (int i = 0; i < 4; i++)
        z_byte(e, ad[i]);
    if (e->zn)
        chunk(out, "IDAT", e->z, e->zn);
    chunk(out, "IEND", NULL, 0);
    au_free(cur);
    au_free(prev);
    au_free(filt);
    au_free(e->buf);
    au_free(e);
    return true;
}
