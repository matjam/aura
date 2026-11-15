/* JPEG decoder: a streaming state machine over input that arrives in
 * pieces of any size.
 *
 * Supported: baseline (SOF0) and extended sequential (SOF1) Huffman, and
 * progressive Huffman (SOF2), 8-bit samples; greyscale, YCbCr, RGB (Adobe
 * transform 0 or component ids 'R','G','B'), CMYK and YCCK (Adobe
 * inverted CMYK, converted to RGB by multiplying by K); any sampling
 * factors 1..4 with at most 10 blocks per MCU; restart intervals with
 * resynchronisation on lost markers; EXIF orientation (APP1 IFD0 tag
 * 0x0112). Rejected cleanly: arithmetic coding, lossless, hierarchical,
 * 12-bit samples, DNL-defined heights, two-component images.
 *
 * Sequential files whose first scan holds every component are decoded
 * MCU row by MCU row and emitted as each row completes (memory O(width)).
 * Progressive files, and sequential files that send components in
 * separate scans, keep coefficients for the whole image and are emitted
 * at EOI (or with what has arrived when input ends early).
 *
 * Scaling: reduced IDCTs give 1/2, 1/4 and 1/8 of the size; each output
 * sample is the exact average of the full-size samples it replaces (the
 * frequencies above N are folded onto the ones they alias to). The
 * generic code does the rest by averaging. A subsampled component gets
 * a larger IDCT when the reduction allows (4:2:0 at 1/2 decodes chroma
 * with 8x8 IDCTs and needs no upsampling). Progressive images keep whole
 * blocks while they fit the coefficient cap (3 x max_bytes), else only
 * the N x N lowest frequencies (then the result is near the average, not
 * on it), else a larger reduction.
 *
 * Chroma is upsampled by replication (as libjpeg does with fancy
 * upsampling off). Corrupt entropy data is decoded on (bad Huffman codes
 * read as zero); blocks with no data are grey. */
#include "image_int.h"

#define JP_EBUF 16384  /* entropy byte buffer */
#define JP_MAXMCU 4096 /* bytes one MCU can use, at most, with any tables */
#define JP_COEF_CAP_X 3 /* coefficient memory cap: 3 x the canvas cap (48 MB by default) */

enum { S_SOI, S_MARKER, S_LEN, S_SKIP, S_SEG, S_SCAN, S_DONE };
enum { CS_GREY, CS_YCC, CS_RGB, CS_CMYK, CS_YCCK };
enum { SCAN_NEED, SCAN_MARKER, SCAN_TRUNC };

/* Natural (row-major) position of each zigzag index. */
static const uint8_t jp_zz[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* Weights of the reduced IDCTs (2^14). The average of the 8-point basis
 * function u over the 8/N pixels a reduced sample covers is a multiple
 * of one N-point basis function: u itself for u < N, else the frequency
 * u aliases to (N=4: 5, 6, 7 fold onto 3, 2, 1; N=2: 3, 5, 7 fold onto
 * 1; 2, 4 and 6 average to zero). The weight is C(u)/2 times that
 * multiple, with its sign, so a reduced IDCT of the folded sums gives
 * exactly the average of the full-size samples. */
static const int16_t jp_w4[8] = {5793, 8035, 7568, 6811, 0, -4551, -3135, -1598};
static const int16_t jp_w2[8] = {5793, 7423, 0, -2607, 0, 1742, 0, -1477};

typedef struct JpHuff {
    uint16_t look[512];   /* 9-bit lookahead: (length << 8) | symbol; length 0: longer code */
    uint32_t maxcode[18]; /* codes of length l are below maxcode[l] (16-bit left-aligned) */
    int delta[17];        /* symbol index = code + delta[length] */
    uint8_t sym[256];
    int nsym;
    bool defined;
} JpHuff;

typedef struct JpBits {
    uint32_t acc; /* bits, left-aligned */
    int cnt;      /* valid bits in acc */
    int pad;      /* zero bytes fed after the end of the data */
    const uint8_t *p, *end;
} JpBits;

typedef struct JpComp {
    int id, h, v, tq;
    int td, ta;     /* Huffman tables in the current scan */
    int dcpred;
    int n;          /* IDCT size (chroma may be larger than luma's, to avoid upsampling) */
    int cs, nc;     /* coefficient store: row stride (8: whole blocks, else n) and size per block */
    int8_t smap[64]; /* zigzag index -> position in a stored block, -1: not kept */
    bool latched;   /* qs holds its quantization table */
    int32_t qs[64]; /* dequantization, natural order (weighted for N < 8) */
    int bw, bh;     /* block grid (non-interleaved scans) */
    int bstride;    /* blocks per row in the coefficient store */
    uint8_t *sbuf;  /* samples of one MCU row */
    int sstride;
    uint8_t *up;    /* a row replicated to the output width */
    int up_sy;      /* source row in up, -1: none */
    int16_t *coef;  /* coefficient store: N*N per block */
    uint32_t *mask; /* N < 8: AC positions that are nonzero, 2 words per block */
} JpComp;

typedef struct Jp {
    int state;
    int marker;
    size_t seglen;
    AuImgHold hold;
    bool ff;

    uint16_t qt[4][64]; /* natural order */
    bool qdef[4];
    JpHuff dc[4], ac[4];
    /* For each AC table, a 9-bit prefix that holds a whole code and its
     * extra bits: (value << 8) | (run << 4) | bits used; 0: none. */
    int32_t acfast[4][512];
    int dri;

    int orient;
    bool exif_seen, adobe;
    int adobe_transform;

    /* Frame. */
    bool have_sof, progressive, began, coefmode;
    int width, height, ncomp;
    JpComp comp[4];
    int hmax, vmax, mcux, mcuy;
    int N, k;   /* luma IDCT size and the reduction it gives */
    int cw, ch; /* size of the rows emitted */
    int colour;
    int nscans;
    uint32_t *argb;
    bool rows_out;   /* streaming: the image is complete */

    /* Scan. */
    int ns, sc[4];
    int ss, se, ah, al;
    bool skip_scan;  /* a scan after the streamed image: data ignored */
    int smx, smy;    /* MCUs across and down */
    int mx, my;
    unsigned rst_left, eobrun;
    int rst_next;
    bool rst_free;   /* the next interval boundary needs no marker (resync) */
    bool starved;    /* no data for the rest of the interval */
    bool scan_done, skip_to_marker;
    bool seg_end;
    int seg_marker;
    uint8_t *eb;
    size_t rp, wp;
    JpBits bits;
    bool bad;
    int16_t blk[64];

    size_t mem;
} Jp;

/* ---- Memory ---- */

static void *jp_alloc(Jp *j, AuImageDecoder *d, size_t n, bool zero)
{
    void *p = zero ? au_calloc(n) : au_alloc(n);
    j->mem += n;
    au_img_mem(d, (ptrdiff_t)n);
    return p;
}

static void jp_release(Jp *j, AuImageDecoder *d, void *p, size_t n)
{
    if (!p)
        return;
    au_free(p);
    j->mem -= n;
    au_img_mem(d, -(ptrdiff_t)n);
}

/* ---- Huffman tables ---- */

/* fast: the AC fast table to fill, NULL for a DC table. */
static bool jp_build_huff(JpHuff *h, int32_t *fast, const uint8_t *counts, const uint8_t *syms, int n)
{
    memset(h->look, 0, sizeof h->look);
    uint32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        int c = counts[l - 1];
        h->delta[l] = k - (int)code;
        if (code + (uint32_t)c > (1u << l))
            return false; /* over-subscribed */
        for (int i = 0; i < c; i++, k++, code++) {
            if (l <= 9) {
                int sh = 9 - l;
                uint16_t e = (uint16_t)(l << 8 | syms[k]);
                for (uint32_t f = 0; f < (1u << sh); f++)
                    h->look[(code << sh) | f] = e;
            }
        }
        h->maxcode[l] = code << (16 - l);
        code <<= 1;
    }
    h->maxcode[17] = 0xFFFFFFFFu;
    memcpy(h->sym, syms, (size_t)n);
    h->nsym = n;
    h->defined = true;
    for (int i = 0; fast && i < 512; i++) {
        unsigned e = h->look[i];
        int l = (int)(e >> 8), rs = (int)(e & 255), r = rs >> 4, s = rs & 15;
        fast[i] = 0;
        if (l && s && l + s <= 9) {
            uint32_t bits = ((uint32_t)i >> (9 - l - s)) & ((1u << s) - 1);
            int v = bits < (1u << (s - 1)) ? (int)bits - (1 << s) + 1 : (int)bits;
            fast[i] = (int32_t)((uint32_t)v << 8) | (r << 4) | (l + s);
        }
    }
    return true;
}

/* ---- Bit reading ---- */

static inline void jp_fill(JpBits *b)
{
    if (b->end - b->p >= 4) {
        while (b->cnt <= 24) {
            b->acc |= (uint32_t)*b->p++ << (24 - b->cnt);
            b->cnt += 8;
        }
        return;
    }
    while (b->cnt <= 24) {
        uint32_t c = 0;
        if (b->p < b->end)
            c = *b->p++;
        else
            b->pad++;
        b->acc |= c << (24 - b->cnt);
        b->cnt += 8;
    }
}

/* n in 1..16 */
static inline uint32_t jp_get(JpBits *b, int n)
{
    if (b->cnt < n)
        jp_fill(b);
    uint32_t v = b->acc >> (32 - n);
    b->acc <<= n;
    b->cnt -= n;
    return v;
}

static inline int jp_bit(JpBits *b)
{
    if (b->cnt < 1)
        jp_fill(b);
    int v = (int)(b->acc >> 31);
    b->acc <<= 1;
    b->cnt--;
    return v;
}

static inline int jp_extend(uint32_t v, int s) { return v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v; }

static inline int jp_huff(JpBits *b, const JpHuff *h, bool *bad)
{
    if (b->cnt < 16)
        jp_fill(b);
    unsigned e = h->look[b->acc >> 23];
    if (e >> 8) {
        int l = (int)(e >> 8);
        b->acc <<= l;
        b->cnt -= l;
        return (int)(e & 255);
    }
    uint32_t t = b->acc >> 16;
    int l = 10;
    while (t >= h->maxcode[l])
        l++;
    if (l > 16) {
        b->acc <<= 16;
        b->cnt -= 16;
        *bad = true;
        return 0;
    }
    int idx = (int)(b->acc >> (32 - l)) + h->delta[l];
    b->acc <<= l;
    b->cnt -= l;
    if (idx < 0 || idx >= h->nsym) {
        *bad = true;
        return 0;
    }
    return h->sym[idx];
}

/* Bits used after the end of the data. */
static inline bool jp_overrun(const JpBits *b) { return b->pad * 8 > b->cnt; }

/* ---- IDCT ----
 * Arithmetic is done in uint32_t, which gives the same bits as signed
 * 32-bit arithmetic for valid data and defined (meaningless) results for
 * hostile coefficients; shifts convert to signed first. */

#define JP_SR(x, n) ((uint32_t)((int32_t)(x) >> (n)))

static inline uint8_t jp_clamp(int32_t v) { return (uint32_t)v > 255 ? (v < 0 ? 0 : 255) : (uint8_t)v; }

static inline uint32_t jp_dq(const int16_t *c, int i, const int32_t *q, int qi)
{
    return (uint32_t)(int32_t)c[i] * (uint32_t)q[qi];
}

/* 8x8, the LL&M integer IDCT (13-bit constants, 2 extra bits between
 * passes). in: coefficients at in[u * cs + v]; q: natural order. */
static void jp_idct8(const int16_t *in, int cs, const int32_t *q, uint8_t *out, int os)
{
    uint32_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int16_t *p = in + c;
        if (!p[cs] && !p[2 * cs] && !p[3 * cs] && !p[4 * cs] && !p[5 * cs] && !p[6 * cs] && !p[7 * cs]) {
            uint32_t dc = jp_dq(p, 0, q, c) * 4u;
            for (int r = 0; r < 8; r++)
                ws[r * 8 + c] = dc;
            continue;
        }
        uint32_t z2 = jp_dq(p, 2 * cs, q, 16 + c), z3 = jp_dq(p, 6 * cs, q, 48 + c);
        uint32_t z1 = (z2 + z3) * 4433u;
        uint32_t t2 = z1 + z3 * (uint32_t)-15137, t3 = z1 + z2 * 6270u;
        z2 = jp_dq(p, 0, q, c);
        z3 = jp_dq(p, 4 * cs, q, 32 + c);
        uint32_t t0 = (z2 + z3) * 8192u, t1 = (z2 - z3) * 8192u;
        uint32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = jp_dq(p, 7 * cs, q, 56 + c);
        t1 = jp_dq(p, 5 * cs, q, 40 + c);
        t2 = jp_dq(p, 3 * cs, q, 24 + c);
        t3 = jp_dq(p, cs, q, 8 + c);
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        uint32_t z4 = t1 + t3, z5 = (z3 + z4) * 9633u;
        t0 *= 2446u;
        t1 *= 16819u;
        t2 *= 25172u;
        t3 *= 12299u;
        z1 *= (uint32_t)-7373;
        z2 *= (uint32_t)-20995;
        z3 = z3 * (uint32_t)-16069 + z5;
        z4 = z4 * (uint32_t)-3196 + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;
        ws[0 * 8 + c] = JP_SR(t10 + t3 + 1024, 11);
        ws[7 * 8 + c] = JP_SR(t10 - t3 + 1024, 11);
        ws[1 * 8 + c] = JP_SR(t11 + t2 + 1024, 11);
        ws[6 * 8 + c] = JP_SR(t11 - t2 + 1024, 11);
        ws[2 * 8 + c] = JP_SR(t12 + t1 + 1024, 11);
        ws[5 * 8 + c] = JP_SR(t12 - t1 + 1024, 11);
        ws[3 * 8 + c] = JP_SR(t13 + t0 + 1024, 11);
        ws[4 * 8 + c] = JP_SR(t13 - t0 + 1024, 11);
    }
    for (int r = 0; r < 8; r++, out += os) {
        const uint32_t *w = ws + r * 8;
        if (!w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) {
            uint8_t v = jp_clamp((int32_t)JP_SR(w[0] + 16, 5) + 128);
            memset(out, v, 8);
            continue;
        }
        uint32_t z2 = w[2], z3 = w[6];
        uint32_t z1 = (z2 + z3) * 4433u;
        uint32_t t2 = z1 + z3 * (uint32_t)-15137, t3 = z1 + z2 * 6270u;
        z2 = w[0] + (128u << 5) + (1u << 4); /* level shift and rounding */
        z3 = w[4];
        uint32_t t0 = (z2 + z3) * 8192u, t1 = (z2 - z3) * 8192u;
        uint32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = w[7];
        t1 = w[5];
        t2 = w[3];
        t3 = w[1];
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        uint32_t z4 = t1 + t3, z5 = (z3 + z4) * 9633u;
        t0 *= 2446u;
        t1 *= 16819u;
        t2 *= 25172u;
        t3 *= 12299u;
        z1 *= (uint32_t)-7373;
        z2 *= (uint32_t)-20995;
        z3 = z3 * (uint32_t)-16069 + z5;
        z4 = z4 * (uint32_t)-3196 + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;
        out[0] = jp_clamp((int32_t)JP_SR(t10 + t3, 18));
        out[7] = jp_clamp((int32_t)JP_SR(t10 - t3, 18));
        out[1] = jp_clamp((int32_t)JP_SR(t11 + t2, 18));
        out[6] = jp_clamp((int32_t)JP_SR(t11 - t2, 18));
        out[2] = jp_clamp((int32_t)JP_SR(t12 + t1, 18));
        out[5] = jp_clamp((int32_t)JP_SR(t12 - t1, 18));
        out[3] = jp_clamp((int32_t)JP_SR(t13 + t0, 18));
        out[4] = jp_clamp((int32_t)JP_SR(t13 - t0, 18));
    }
}

/* 4-point IDCT of b0..b3 (13-bit cosines). */
#define JP_IDCT4(b0, b1, b2, b3, y0, y1, y2, y3)                                                                  \
    do {                                                                                                          \
        uint32_t e0_ = (b0) * 8192u + (b2) * 5793u, e1_ = (b0) * 8192u - (b2) * 5793u;                            \
        uint32_t o0_ = (b1) * 7568u + (b3) * 3135u, o1_ = (b1) * 3135u - (b3) * 7568u;                            \
        y0 = e0_ + o0_;                                                                                           \
        y3 = e0_ - o0_;                                                                                           \
        y1 = e1_ + o1_;                                                                                           \
        y2 = e1_ - o1_;                                                                                           \
    } while (0)

/* 4x4. q is weighted (2^8 units). full: in is a whole 8x8 block, and
 * the frequencies above 4 are folded in (an exact 2x2 average); else
 * only the 4x4 lowest frequencies exist (cs is 4). */
static void jp_idct4(const int16_t *in, int cs, const int32_t *q, uint8_t *out, int os, bool full)
{
    uint32_t ws[32]; /* 4 rows of 8 columns */
    int ncol = full ? 8 : 4;
    for (int c = 0; c < ncol; c++) {
        if (c == 4)
            continue;
        uint32_t b0 = jp_dq(in, c, q, c), b1 = jp_dq(in, cs + c, q, 8 + c);
        uint32_t b2 = jp_dq(in, 2 * cs + c, q, 16 + c), b3 = jp_dq(in, 3 * cs + c, q, 24 + c);
        if (full) {
            b1 += jp_dq(in, 7 * cs + c, q, 56 + c);
            b2 += jp_dq(in, 6 * cs + c, q, 48 + c);
            b3 += jp_dq(in, 5 * cs + c, q, 40 + c);
        }
        uint32_t y0, y1, y2, y3;
        JP_IDCT4(b0, b1, b2, b3, y0, y1, y2, y3);
        ws[0 + c] = JP_SR(y0 + 4096, 13);
        ws[8 + c] = JP_SR(y1 + 4096, 13);
        ws[16 + c] = JP_SR(y2 + 4096, 13);
        ws[24 + c] = JP_SR(y3 + 4096, 13);
    }
    for (int r = 0; r < 4; r++, out += os) {
        const uint32_t *w = ws + r * 8;
        uint32_t b0 = w[0] + (128u << 8) + 128u, b1 = w[1], b2 = w[2], b3 = w[3];
        if (full) {
            b1 += w[7];
            b2 += w[6];
            b3 += w[5];
        }
        uint32_t y0, y1, y2, y3;
        JP_IDCT4(b0, b1, b2, b3, y0, y1, y2, y3);
        out[0] = jp_clamp((int32_t)JP_SR(y0, 21));
        out[1] = jp_clamp((int32_t)JP_SR(y1, 21));
        out[2] = jp_clamp((int32_t)JP_SR(y2, 21));
        out[3] = jp_clamp((int32_t)JP_SR(y3, 21));
    }
}

/* 2x2; full folds frequencies 3, 5 and 7 onto 1 (an exact 4x4 average). */
static void jp_idct2(const int16_t *in, int cs, const int32_t *q, uint8_t *out, int os, bool full)
{
    uint32_t w0[8], w1[8];
    int ncol = full ? 8 : 2;
    for (int c = 0; c < ncol; c++) {
        if (c > 1 && !(c & 1))
            continue;
        uint32_t b0 = jp_dq(in, c, q, c), b1 = jp_dq(in, cs + c, q, 8 + c);
        if (full)
            b1 += jp_dq(in, 3 * cs + c, q, 24 + c) + jp_dq(in, 5 * cs + c, q, 40 + c) +
                  jp_dq(in, 7 * cs + c, q, 56 + c);
        /* 5793 = cos(pi/4) in 2^13 */
        w0[c] = JP_SR(b0 * 8192u + b1 * 5793u + 4096, 13);
        w1[c] = JP_SR(b0 * 8192u - b1 * 5793u + 4096, 13);
    }
    for (int r = 0; r < 2; r++, out += os) {
        const uint32_t *w = r ? w1 : w0;
        uint32_t b0 = w[0] + (128u << 8) + 128u, b1 = w[1];
        if (full)
            b1 += w[3] + w[5] + w[7];
        out[0] = jp_clamp((int32_t)JP_SR(b0 * 8192u + b1 * 5793u, 21));
        out[1] = jp_clamp((int32_t)JP_SR(b0 * 8192u - b1 * 5793u, 21));
    }
}

/* The value of every sample of a block with only a DC coefficient. */
static inline uint8_t jp_dc_value(const JpComp *c, int dc)
{
    uint32_t v = (uint32_t)(int32_t)dc * (uint32_t)c->qs[0];
    if (c->n == 8)
        return jp_clamp((int32_t)JP_SR(v + 4, 3) + 128);
    return jp_clamp((int32_t)JP_SR(v + (128u << 8) + 128u, 8));
}

static void jp_fill_block(int n, uint8_t v, uint8_t *out, int os)
{
    for (int r = 0; r < n; r++, out += os)
        memset(out, v, (size_t)n);
}

/* Transforms one block to n x n samples: a whole 8x8 block (cs 8) or a
 * stored block of the n x n lowest frequencies (cs n). */
static void jp_idct(const JpComp *c, const int16_t *in, int cs, bool dc_only, uint8_t *out, int os)
{
    if (dc_only) {
        jp_fill_block(c->n, jp_dc_value(c, in[0]), out, os);
        return;
    }
    switch (c->n) {
    case 8: jp_idct8(in, cs, c->qs, out, os); break;
    case 4: jp_idct4(in, cs, c->qs, out, os, cs == 8); break;
    case 2: jp_idct2(in, cs, c->qs, out, os, cs == 8); break;
    default: *out = jp_dc_value(c, in[0]); break;
    }
}

/* Copies a component's quantization table into its dequantization
 * table, weighted for the reduced IDCTs. */
static void jp_latch(Jp *j, JpComp *c)
{
    const uint16_t *q = j->qt[c->tq];
    const int16_t *w = c->n == 4 ? jp_w4 : jp_w2;
    for (int i = 0; i < 64; i++) {
        if (c->n == 8) {
            c->qs[i] = q[i];
        } else if (c->n == 1) {
            c->qs[i] = i ? 0 : (int32_t)q[0] * 32; /* (C(0)/2)^2 = 1/8, in 2^8 units */
        } else {
            int64_t v = (int64_t)q[i] * w[i >> 3] * w[i & 7];
            c->qs[i] = (int32_t)(v >= 0 ? (v + (1 << 19)) >> 20 : -((-v + (1 << 19)) >> 20));
        }
    }
    c->latched = true;
}

/* ---- Output ---- */

/* Replicates a row of h samples per hmax output pixels to w pixels. */
static void jp_upsample_row(const uint8_t *src, uint8_t *dst, int w, int h, int hmax)
{
    if (hmax == 2 * h) {
        int x = 0;
        for (; x + 1 < w; x += 2)
            dst[x] = dst[x + 1] = src[x >> 1];
        if (x < w)
            dst[x] = src[x >> 1];
    } else if (hmax == 4 * h) {
        for (int x = 0; x < w; x++)
            dst[x] = src[x >> 2];
    } else {
        for (int x = 0; x < w; x++)
            dst[x] = src[(x * h) / hmax];
    }
}

static inline uint32_t jp_rgb(int r, int g, int b)
{
    return 0xFF000000u | (uint32_t)jp_clamp(r) << 16 | (uint32_t)jp_clamp(g) << 8 | jp_clamp(b);
}

static inline int jp_mul255(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/* Converts and emits the rows of MCU row my. */
static void jp_emit_mcu_row(Jp *j, AuImageDecoder *d, int my)
{
    int rows = j->vmax * j->N, y0 = my * rows;
    int w = j->cw;
    for (int c = 0; c < j->ncomp; c++)
        j->comp[c].up_sy = -1;
    for (int yy = 0; yy < rows && y0 + yy < j->ch; yy++) {
        const uint8_t *src[4];
        for (int c = 0; c < j->ncomp; c++) {
            JpComp *cp = &j->comp[c];
            int vs = cp->v * cp->n, hs = cp->h * cp->n;
            int sy = vs == rows ? yy : (yy * vs) / rows;
            const uint8_t *row = cp->sbuf + (size_t)sy * (size_t)cp->sstride;
            if (hs == j->hmax * j->N) {
                src[c] = row;
            } else {
                if (cp->up_sy != sy) {
                    jp_upsample_row(row, cp->up, w, hs, j->hmax * j->N);
                    cp->up_sy = sy;
                }
                src[c] = cp->up;
            }
        }
        uint32_t *o = j->argb;
        switch (j->colour) {
        case CS_GREY:
            for (int x = 0; x < w; x++)
                o[x] = 0xFF000000u | (uint32_t)src[0][x] * 0x010101u;
            break;
        case CS_RGB:
            for (int x = 0; x < w; x++)
                o[x] = 0xFF000000u | (uint32_t)src[0][x] << 16 | (uint32_t)src[1][x] << 8 | src[2][x];
            break;
        case CS_YCC:
            for (int x = 0; x < w; x++) {
                int y = src[0][x], cb = src[1][x] - 128, cr = src[2][x] - 128;
                o[x] = jp_rgb(y + ((91881 * cr + 32768) >> 16), y + ((-22554 * cb - 46802 * cr + 32768) >> 16),
                              y + ((116130 * cb + 32768) >> 16));
            }
            break;
        case CS_CMYK:
            for (int x = 0; x < w; x++) {
                int cc = src[0][x], m = src[1][x], ye = src[2][x], k = src[3][x];
                if (!j->adobe) {
                    cc = 255 - cc;
                    m = 255 - m;
                    ye = 255 - ye;
                    k = 255 - k;
                }
                o[x] = jp_rgb(jp_mul255(cc, k), jp_mul255(m, k), jp_mul255(ye, k));
            }
            break;
        default: /* YCCK: YCbCr gives inverted C, M, Y */
            for (int x = 0; x < w; x++) {
                int y = src[0][x], cb = src[1][x] - 128, cr = src[2][x] - 128, k = src[3][x];
                int r = jp_clamp(y + ((91881 * cr + 32768) >> 16));
                int g = jp_clamp(y + ((-22554 * cb - 46802 * cr + 32768) >> 16));
                int b = jp_clamp(y + ((116130 * cb + 32768) >> 16));
                o[x] = jp_rgb(jp_mul255(255 - r, k), jp_mul255(255 - g, k), jp_mul255(255 - b, k));
            }
            break;
        }
        au_img_row(d, y0 + yy, o);
    }
}

/* The output pass of coefficient mode: transforms every block and emits
 * every row. */
static void jp_output_all(Jp *j, AuImageDecoder *d)
{
    for (int my = 0; my < j->mcuy; my++) {
        for (int c = 0; c < j->ncomp; c++) {
            JpComp *cp = &j->comp[c];
            int N = cp->n, nc = cp->nc;
            int bx_n = j->mcux * cp->h;
            for (int by = 0; by < cp->v; by++) {
                const int16_t *blk = cp->coef + ((size_t)(my * cp->v + by) * (size_t)cp->bstride) * (size_t)nc;
                uint8_t *out = cp->sbuf + (size_t)(by * N) * (size_t)cp->sstride;
                for (int bx = 0; bx < bx_n; bx++, blk += nc, out += N) {
                    bool dc_only = true;
                    for (int i = 1; i < nc && dc_only; i++)
                        if (blk[i])
                            dc_only = false;
                    jp_idct(cp, blk, cp->cs, dc_only, out, cp->sstride);
                }
            }
        }
        jp_emit_mcu_row(j, d, my);
    }
}

static void jp_free_coefs(Jp *j, AuImageDecoder *d)
{
    for (int c = 0; c < j->ncomp; c++) {
        JpComp *cp = &j->comp[c];
        size_t blocks = (size_t)cp->bstride * (size_t)(j->mcuy * cp->v);
        if (cp->coef)
            jp_release(j, d, cp->coef, blocks * (size_t)cp->nc * sizeof(int16_t));
        if (cp->mask)
            jp_release(j, d, cp->mask, blocks * 2 * sizeof(uint32_t));
        cp->coef = NULL;
        cp->mask = NULL;
    }
}

/* ---- Segments ---- */

static bool jp_parse_dqt(Jp *j, const uint8_t *p, size_t n)
{
    while (n) {
        int pq = p[0] >> 4, tq = p[0] & 15;
        if (pq > 1 || tq > 3)
            return false;
        size_t need = 1 + 64 * (size_t)(pq + 1);
        if (n < need)
            return false;
        for (int i = 0; i < 64; i++)
            j->qt[tq][jp_zz[i]] = (uint16_t)(pq ? au_img_be16(p + 1 + 2 * i) : p[1 + i]);
        j->qdef[tq] = true;
        p += need;
        n -= need;
    }
    return true;
}

static bool jp_parse_dht(Jp *j, const uint8_t *p, size_t n)
{
    while (n) {
        if (n < 17)
            return false;
        int tc = p[0] >> 4, th = p[0] & 15;
        if (tc > 1 || th > 3)
            return false;
        int total = 0;
        for (int i = 0; i < 16; i++)
            total += p[1 + i];
        if (total > 256 || n < 17 + (size_t)total)
            return false;
        if (!jp_build_huff(tc ? &j->ac[th] : &j->dc[th], tc ? j->acfast[th] : NULL, p + 1, p + 17, total))
            return false;
        p += 17 + total;
        n -= 17 + (size_t)total;
    }
    return true;
}

static void jp_parse_exif(Jp *j, const uint8_t *p, size_t n)
{
    if (j->exif_seen || n < 6 || memcmp(p, "Exif\0", 5))
        return;
    j->exif_seen = true;
    p += 6;
    n -= 6;
    if (n < 8)
        return;
    bool le;
    if (p[0] == 'I' && p[1] == 'I')
        le = true;
    else if (p[0] == 'M' && p[1] == 'M')
        le = false;
    else
        return;
#define RD16(q) (le ? au_img_le16(q) : au_img_be16(q))
#define RD32(q) (le ? au_img_le32(q) : au_img_be32(q))
    if (RD16(p + 2) != 42)
        return;
    uint32_t ifd = RD32(p + 4);
    if (ifd > n || n - ifd < 2)
        return;
    uint32_t count = RD16(p + ifd);
    if ((n - ifd - 2) / 12 < count)
        return;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + ifd + 2 + 12 * i;
        if (RD16(e) == 0x0112) {
            uint32_t type = RD16(e + 2), cnt = RD32(e + 4);
            uint32_t v = type == 3 ? RD16(e + 8) : type == 4 ? RD32(e + 8) : 0;
            if (cnt >= 1 && v >= 1 && v <= 8)
                j->orient = (int)v;
            break;
        }
    }
#undef RD16
#undef RD32
}

static AuImageStatus jp_parse_sof(Jp *j, AuImageDecoder *d, const uint8_t *p, size_t n)
{
    if (j->have_sof)
        return au_img_fail(d, "corrupt JPEG: two frames");
    if (n < 6)
        return au_img_fail(d, "corrupt JPEG frame header");
    if (p[0] != 8)
        return au_img_fail(d, "unsupported JPEG precision");
    int h = (int)au_img_be16(p + 1), w = (int)au_img_be16(p + 3), nf = p[5];
    if (h == 0)
        return au_img_fail(d, "unsupported JPEG (DNL height)");
    if (w == 0)
        return au_img_fail(d, "zero size");
    if (nf != 1 && nf != 3 && nf != 4)
        return au_img_fail(d, "unsupported JPEG component count");
    if (n < 6 + 3 * (size_t)nf)
        return au_img_fail(d, "corrupt JPEG frame header");
    j->hmax = j->vmax = 1;
    for (int i = 0; i < nf; i++) {
        JpComp *c = &j->comp[i];
        const uint8_t *q = p + 6 + 3 * i;
        c->id = q[0];
        c->h = q[1] >> 4;
        c->v = q[1] & 15;
        c->tq = q[2];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
            return au_img_fail(d, "corrupt JPEG frame header");
        for (int k = 0; k < i; k++)
            if (j->comp[k].id == c->id)
                return au_img_fail(d, "corrupt JPEG: duplicate component");
        if (nf == 1)
            c->h = c->v = 1; /* one component: blocks are the MCU */
        if (c->h > j->hmax)
            j->hmax = c->h;
        if (c->v > j->vmax)
            j->vmax = c->v;
    }
    j->ncomp = nf;
    j->width = w;
    j->height = h;
    j->have_sof = true;
    j->mcux = (w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < nf; i++) {
        JpComp *c = &j->comp[i];
        int cw = (int)(((long)w * c->h + j->hmax - 1) / j->hmax), ch = (int)(((long)h * c->v + j->vmax - 1) / j->vmax);
        c->bw = (cw + 7) / 8;
        c->bh = (ch + 7) / 8;
        c->bstride = j->mcux * c->h;
    }
    if (nf == 1)
        j->colour = CS_GREY;
    else if (nf == 3) {
        if (j->adobe)
            j->colour = j->adobe_transform ? CS_YCC : CS_RGB;
        else if (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B')
            j->colour = CS_RGB;
        else
            j->colour = CS_YCC;
    } else {
        j->colour = j->adobe && j->adobe_transform == 2 ? CS_YCCK : CS_CMYK;
    }
    if (!au_img_header(d, w, h, j->orient, false))
        return AU_IMAGE_ERROR;
    return AU_IMAGE_MORE;
}

/* Allocates the output and the decoding buffers at the first scan. */
static bool jp_begin(Jp *j, AuImageDecoder *d)
{
    int s = au_img_scale(d);
    int k = s < 8 ? s : 8;
    /* Coefficient mode keeps whole blocks when they fit the cap (the
     * reduced IDCTs then give exact averages), else only the n x n lowest
     * frequencies, else a larger reduction. */
    bool whole = true;
    for (;;) {
        int N = j->N = 8 / k;
        /* A subsampled component gets a larger IDCT when the reduction
         * allows, which saves upsampling it (as libjpeg does). */
        for (int c = 0; c < j->ncomp; c++) {
            JpComp *cp = &j->comp[c];
            int n = N;
            while (n < 8 && (j->hmax * N) % (cp->h * n * 2) == 0 && (j->vmax * N) % (cp->v * n * 2) == 0)
                n *= 2;
            cp->n = n;
            cp->cs = whole && n > 1 ? 8 : n; /* DC alone is the exact 8x8 average */
            cp->nc = cp->cs * cp->cs;
        }
        if (!j->coefmode)
            break;
        size_t total = 0;
        bool ok = true;
        for (int c = 0; c < j->ncomp && ok; c++) {
            JpComp *cp = &j->comp[c];
            size_t blocks, per, bytes;
            per = (size_t)cp->nc * sizeof(int16_t) + (cp->cs < 8 ? 2 * sizeof(uint32_t) : 0);
            ok = au_img_mul((size_t)cp->bstride, (size_t)j->mcuy * (size_t)cp->v, &blocks) &&
                 au_img_mul(blocks, per, &bytes) && au_img_add(total, bytes, &total);
        }
        size_t cap = d->opt.max_bytes > (size_t)-1 / JP_COEF_CAP_X ? (size_t)-1 : d->opt.max_bytes * JP_COEF_CAP_X;
        if (ok && total <= cap)
            break;
        if (whole && k > 1) {
            whole = false;
            continue;
        }
        if (k == 8)
            return au_img_fail(d, "image too large"), false;
        k *= 2;
    }
    j->k = k;
    j->cw = (j->width + k - 1) / k;
    j->ch = (j->height + k - 1) / k;
    int red = s / k;
    if (red < 1)
        red = 1;
    if (!au_img_begin(d, j->cw, j->ch, red))
        return false;
    for (int c = 0; c < j->ncomp; c++) {
        JpComp *cp = &j->comp[c];
        int n = cp->n, cs = cp->cs;
        memset(cp->smap, -1, sizeof cp->smap);
        for (int i = 0; i < 64; i++) {
            int u = jp_zz[i] >> 3, v = jp_zz[i] & 7;
            if (u < cs && v < cs)
                cp->smap[i] = (int8_t)(u * cs + v);
        }
        size_t sw = (size_t)j->mcux * (size_t)cp->h * (size_t)n, sh = (size_t)cp->v * (size_t)n;
        cp->sstride = (int)sw;
        cp->sbuf = jp_alloc(j, d, sw * sh, false);
        if (cp->h * n != j->hmax * j->N)
            cp->up = jp_alloc(j, d, (size_t)j->cw, false);
        if (j->coefmode) {
            size_t blocks = (size_t)cp->bstride * (size_t)(j->mcuy * cp->v);
            cp->coef = jp_alloc(j, d, blocks * (size_t)cp->nc * sizeof(int16_t), true);
            if (cs < 8 && j->progressive)
                cp->mask = jp_alloc(j, d, blocks * 2 * sizeof(uint32_t), true);
        }
    }
    j->argb = jp_alloc(j, d, (size_t)j->cw * 4, false);
    j->eb = jp_alloc(j, d, JP_EBUF, false);
    j->began = true;
    return true;
}

static AuImageStatus jp_parse_sos(Jp *j, AuImageDecoder *d, const uint8_t *p, size_t n)
{
    if (!j->have_sof)
        return au_img_fail(d, "corrupt JPEG: scan before frame");
    if (n < 1)
        return au_img_fail(d, "corrupt JPEG scan header");
    int ns = p[0];
    if (ns < 1 || ns > 4 || n < 4 + 2 * (size_t)ns)
        return au_img_fail(d, "corrupt JPEG scan header");
    int blocks = 0;
    for (int i = 0; i < ns; i++) {
        int id = p[1 + 2 * i], t = p[2 + 2 * i], c;
        for (c = 0; c < j->ncomp; c++)
            if (j->comp[c].id == id)
                break;
        if (c == j->ncomp)
            return au_img_fail(d, "corrupt JPEG: unknown component");
        for (int k = 0; k < i; k++)
            if (j->sc[k] == c)
                return au_img_fail(d, "corrupt JPEG: duplicate component");
        j->sc[i] = c;
        j->comp[c].td = t >> 4;
        j->comp[c].ta = t & 15;
        if (j->comp[c].td > 3 || j->comp[c].ta > 3)
            return au_img_fail(d, "corrupt JPEG scan header");
        blocks += j->comp[c].h * j->comp[c].v;
    }
    if (ns > 1 && blocks > 10)
        return au_img_fail(d, "corrupt JPEG: MCU too large");
    const uint8_t *q = p + 1 + 2 * ns;
    j->ns = ns;
    j->ss = q[0];
    j->se = q[1];
    j->ah = q[2] >> 4;
    j->al = q[2] & 15;
    if (j->progressive) {
        if (j->ss > 63 || j->se > 63 || j->ss > j->se || (j->ss == 0 && j->se != 0) || (j->ss > 0 && ns != 1) ||
            j->ah > 13 || j->al > 13)
            return au_img_fail(d, "corrupt JPEG: bad progression");
    } else {
        j->ss = 0;
        j->se = 63;
        j->ah = j->al = 0;
    }
    j->skip_scan = false;
    if (!j->began) {
        j->coefmode = j->progressive || ns != j->ncomp;
        if (!jp_begin(j, d))
            return AU_IMAGE_ERROR;
    } else if (!j->coefmode) {
        j->skip_scan = true; /* the image was streamed by the first scan */
    }
    if (!j->skip_scan) {
        for (int i = 0; i < ns; i++) {
            JpComp *c = &j->comp[j->sc[i]];
            bool need_dc = !j->progressive || (j->ss == 0 && j->ah == 0);
            bool need_ac = !j->progressive || j->ss > 0;
            if ((need_dc && !j->dc[c->td].defined) || (need_ac && !j->ac[c->ta].defined))
                return au_img_fail(d, "corrupt JPEG: missing Huffman table");
            if (!c->latched) {
                if (!j->qdef[c->tq])
                    return au_img_fail(d, "corrupt JPEG: missing quantization table");
                jp_latch(j, c);
            }
            c->dcpred = 0;
        }
    }
    if (ns == 1) {
        JpComp *c = &j->comp[j->sc[0]];
        j->smx = c->bw;
        j->smy = c->bh;
    } else {
        j->smx = j->mcux;
        j->smy = j->mcuy;
    }
    j->nscans++;
    j->mx = j->my = 0;
    j->rst_left = j->dri ? (unsigned)j->dri : 0;
    j->rst_next = 0;
    j->rst_free = false;
    j->eobrun = 0;
    j->starved = false;
    j->scan_done = j->skip_scan;
    j->skip_to_marker = j->skip_scan;
    j->seg_end = false;
    j->ff = false;
    j->rp = j->wp = 0;
    memset(&j->bits, 0, sizeof j->bits);
    return AU_IMAGE_MORE;
}

/* ---- Entropy-coded data ---- */

/* Moves entropy-coded bytes from the input into the buffer, removing
 * stuffing, until the buffer is full, a marker ends the data or the
 * input is used up. */
static void jp_copy(Jp *j, const uint8_t **in, size_t *len)
{
    if (j->seg_end)
        return;
    if (j->rp && (j->rp == j->wp || j->rp >= JP_EBUF / 2)) {
        memmove(j->eb, j->eb + j->rp, j->wp - j->rp);
        j->wp -= j->rp;
        j->rp = 0;
    }
    const uint8_t *p = *in, *e = p + *len;
    bool skip = j->skip_to_marker;
    uint8_t *eb = j->eb;
    size_t wp = j->wp;
    while (p < e) {
        if (j->ff) {
            uint8_t c = *p++;
            if (c == 0xFF)
                continue;
            j->ff = false;
            if (c == 0) {
                if (!skip)
                    eb[wp++] = 0xFF;
                continue;
            }
            if (c >= 0xD0 && c <= 0xD7 && (!j->dri || j->scan_done))
                continue; /* a restart marker where none is expected */
            j->seg_end = true;
            j->seg_marker = c;
            break;
        }
        if (!skip && wp >= JP_EBUF)
            break;
        const uint8_t *f = memchr(p, 0xFF, (size_t)(e - p));
        size_t n = (size_t)((f ? f : e) - p);
        if (!skip) {
            if (n > JP_EBUF - wp)
                n = JP_EBUF - wp;
            memcpy(eb + wp, p, n);
            wp += n;
        }
        p += n;
        if (p < e && *p == 0xFF && (skip || wp < JP_EBUF)) {
            j->ff = true;
            p++;
        }
    }
    j->wp = wp;
    *len -= (size_t)(p - *in);
    *in = p;
}

static inline bool jp_nz(const int8_t *smap, const int16_t *cb, const uint32_t *mb, int k)
{
    return mb ? (mb[k >> 5] >> (k & 31)) & 1 : cb[smap[k]] != 0;
}

static inline void jp_refine(const int8_t *smap, int16_t *cb, int k, int p1)
{
    int si = smap[k];
    if (si >= 0 && !(cb[si] & p1))
        cb[si] = (int16_t)(cb[si] + (cb[si] >= 0 ? p1 : -p1));
}

/* Decodes a sequential block into j->blk (natural order). Returns false
 * when only DC is set. */
static bool jp_seq_block(Jp *j, JpBits *b, JpComp *c)
{
    int16_t *blk = j->blk;
    memset(blk, 0, sizeof j->blk);
    int s = jp_huff(b, &j->dc[c->td], &j->bad);
    int diff = 0;
    if (s) {
        if (s > 16) {
            j->bad = true;
            s = 16;
        }
        diff = jp_extend(jp_get(b, s), s);
    }
    c->dcpred = (int16_t)(c->dcpred + diff);
    blk[0] = (int16_t)c->dcpred;
    const JpHuff *ac = &j->ac[c->ta];
    const int32_t *fast = j->acfast[c->ta];
    bool any = false;
    for (int k = 1; k < 64; k++) {
        if (b->cnt < 16)
            jp_fill(b);
        int32_t fa = fast[b->acc >> 23];
        if (fa) {
            /* Consume before deciding anything: a speculative decode
             * must not act on bits it did not use. */
            b->acc <<= fa & 15;
            b->cnt -= fa & 15;
            k += (fa >> 4) & 15;
            if (k > 63) {
                j->bad = true;
                break;
            }
            blk[jp_zz[k]] = (int16_t)(fa >> 8);
            any = true;
            continue;
        }
        int rs = jp_huff(b, ac, &j->bad);
        int r = rs >> 4;
        s = rs & 15;
        if (s) {
            k += r;
            if (k > 63) {
                j->bad = true;
                break;
            }
            blk[jp_zz[k]] = (int16_t)jp_extend(jp_get(b, s), s);
            any = true;
        } else {
            if (r != 15)
                break;
            k += 15;
        }
    }
    return any;
}

static void jp_prog_block(Jp *j, JpBits *b, JpComp *c, int16_t *cb, uint32_t *mb)
{
    if (j->ss == 0) {
        if (j->ah == 0) {
            int s = jp_huff(b, &j->dc[c->td], &j->bad), diff = 0;
            if (s) {
                if (s > 16) {
                    j->bad = true;
                    s = 16;
                }
                diff = jp_extend(jp_get(b, s), s);
            }
            c->dcpred = (int16_t)(c->dcpred + diff);
            cb[0] = (int16_t)(c->dcpred * (1 << j->al));
        } else if (jp_bit(b)) {
            cb[0] = (int16_t)(cb[0] | (1 << j->al));
        }
        return;
    }
    const JpHuff *h = &j->ac[c->ta];
    const int8_t *smap = c->smap;
    int se = j->se;
    if (j->ah == 0) {
        if (j->eobrun) {
            j->eobrun--;
            return;
        }
        for (int k = j->ss; k <= se; k++) {
            int rs = jp_huff(b, h, &j->bad), r = rs >> 4, s = rs & 15;
            if (s) {
                k += r;
                if (k > se) {
                    j->bad = true;
                    break;
                }
                int v = jp_extend(jp_get(b, s), s) * (1 << j->al);
                int si = smap[k];
                if (si >= 0)
                    cb[si] = (int16_t)v;
                if (mb)
                    mb[k >> 5] |= 1u << (k & 31);
            } else {
                if (r < 15) {
                    j->eobrun = 1u << r;
                    if (r)
                        j->eobrun += jp_get(b, r);
                    j->eobrun--;
                    break;
                }
                k += 15;
            }
        }
        return;
    }
    int p1 = 1 << j->al, k = j->ss;
    if (!j->eobrun) {
        for (; k <= se; k++) {
            int rs = jp_huff(b, h, &j->bad), r = rs >> 4, s = rs & 15, v = 0;
            if (s) {
                if (s != 1)
                    j->bad = true;
                v = jp_bit(b) ? p1 : -p1;
            } else if (r != 15) {
                j->eobrun = 1u << r;
                if (r)
                    j->eobrun += jp_get(b, r);
                break;
            }
            while (k <= se) {
                if (jp_nz(smap, cb, mb, k)) {
                    if (jp_bit(b))
                        jp_refine(smap, cb, k, p1);
                } else if (--r < 0) {
                    break;
                }
                k++;
            }
            if (v && k <= se) {
                int si = smap[k];
                if (si >= 0)
                    cb[si] = (int16_t)v;
                if (mb)
                    mb[k >> 5] |= 1u << (k & 31);
            }
        }
    }
    if (j->eobrun) {
        for (; k <= se; k++)
            if (jp_nz(smap, cb, mb, k) && jp_bit(b))
                jp_refine(smap, cb, k, p1);
        j->eobrun--;
    }
}

/* One block of component c at block position (bx, by) of its grid; ry is
 * its block row inside the MCU row (streaming). */
static void jp_block(Jp *j, JpBits *b, JpComp *c, int bx, int by, int ry)
{
    int n = c->n;
    if (!j->coefmode) {
        uint8_t *out = c->sbuf + (size_t)(ry * n) * (size_t)c->sstride + (size_t)bx * (size_t)n;
        if (j->starved) {
            jp_fill_block(n, 128, out, c->sstride);
            return;
        }
        bool any = jp_seq_block(j, b, c);
        jp_idct(c, j->blk, 8, !any, out, c->sstride);
        return;
    }
    if (j->starved)
        return;
    size_t bi = (size_t)by * (size_t)c->bstride + (size_t)bx;
    int16_t *cb = c->coef + bi * (size_t)c->nc;
    if (!j->progressive) {
        jp_seq_block(j, b, c);
        for (int i = 0; i < 64; i++)
            if (c->smap[i] >= 0)
                cb[c->smap[i]] = j->blk[jp_zz[i]];
        return;
    }
    jp_prog_block(j, b, c, cb, c->mask ? c->mask + 2 * bi : NULL);
}

static void jp_decode_mcu(Jp *j)
{
    JpBits b = j->bits;
    b.p = j->eb + j->rp;
    b.end = j->eb + j->wp;
    if (j->ns == 1) {
        JpComp *c = &j->comp[j->sc[0]];
        jp_block(j, &b, c, j->mx, j->my, 0);
    } else {
        for (int i = 0; i < j->ns; i++) {
            JpComp *c = &j->comp[j->sc[i]];
            for (int y = 0; y < c->v; y++)
                for (int x = 0; x < c->h; x++)
                    jp_block(j, &b, c, j->mx * c->h + x, j->my * c->v + y, y);
        }
    }
    if (jp_overrun(&b)) {
        if (j->seg_end)
            j->starved = true;
    } else if (b.pad) {
        /* Zero bytes fed past the end were not used: take them out, so
         * that data arriving later is read in their place. */
        b.cnt -= b.pad * 8;
        b.pad = 0;
    }
    j->rp = (size_t)(b.p - j->eb);
    j->bits = b;
}

static void jp_restart(Jp *j)
{
    memset(&j->bits, 0, sizeof j->bits);
    j->rp = j->wp = 0;
    j->ff = false;
    j->seg_end = false;
    j->skip_to_marker = false;
    j->starved = false;
    j->eobrun = 0;
    for (int i = 0; i < j->ns; i++)
        j->comp[j->sc[i]].dcpred = 0;
    j->rst_left = (unsigned)j->dri;
}

/* Advances past the MCU just decoded; emits a completed MCU row. */
static void jp_advance(Jp *j, AuImageDecoder *d)
{
    if (j->dri)
        j->rst_left--;
    if (++j->mx < j->smx)
        return;
    j->mx = 0;
    if (!j->coefmode)
        jp_emit_mcu_row(j, d, j->my);
    if (++j->my >= j->smy) {
        j->scan_done = true;
        j->skip_to_marker = true;
        if (!j->coefmode)
            j->rows_out = true;
    }
}

/* Runs the entropy-coded data of a scan. */
static int jp_scan(Jp *j, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    for (;;) {
        jp_copy(j, in, len);
        bool end_in = final && *len == 0 && !j->seg_end;
        if (j->scan_done) {
            if (j->seg_end) {
                j->marker = j->seg_marker;
                return SCAN_MARKER;
            }
            if (!*len)
                return SCAN_NEED;
            continue;
        }
        while (!j->scan_done) {
            if (j->dri && j->rst_left == 0) {
                /* An interval boundary. */
                if (j->rst_free) {
                    j->rst_free = false;
                    j->starved = false;
                    j->rst_left = (unsigned)j->dri;
                } else if (j->seg_end) {
                    int m = j->seg_marker;
                    if (m >= 0xD0 && m <= 0xD7) {
                        int delta = (m - 0xD0 - j->rst_next) & 7;
                        jp_restart(j);
                        if (delta >= 1 && delta <= 3) {
                            /* Intervals were lost: leave them grey and
                             * resume with this marker's data after them. */
                            j->starved = true;
                            j->rst_left = (unsigned)j->dri * (unsigned)delta;
                            j->rst_free = true;
                            j->rst_next = (m - 0xD0 + 1) & 7;
                        } else {
                            j->rst_next = (j->rst_next + 1) & 7;
                        }
                        break; /* get the next interval's data */
                    }
                    j->starved = true; /* the data ended: the rest is grey */
                    j->rst_left = 0xFFFFFFFFu;
                } else if (end_in) {
                    goto truncated;
                } else {
                    j->rp = j->wp = 0;
                    j->skip_to_marker = true;
                    break;
                }
            }
            if (!j->starved && !j->seg_end) {
                size_t avail = j->wp - j->rp;
                if (end_in) {
                    if (jp_overrun(&j->bits) || (!avail && j->bits.cnt == 0))
                        goto truncated;
                } else if (avail < JP_MAXMCU) {
                    if (j->coefmode)
                        break;
                    /* Streaming: try the MCU with the data there is, and
                     * undo it if it needed more, so rows appear as soon
                     * as their data has arrived. */
                    JpBits sb = j->bits;
                    size_t srp = j->rp;
                    int pred[4];
                    for (int i = 0; i < j->ns; i++)
                        pred[i] = j->comp[j->sc[i]].dcpred;
                    jp_decode_mcu(j);
                    if (jp_overrun(&j->bits)) {
                        j->bits = sb;
                        j->rp = srp;
                        for (int i = 0; i < j->ns; i++)
                            j->comp[j->sc[i]].dcpred = pred[i];
                        break;
                    }
                    jp_advance(j, d);
                    continue;
                }
            }
            jp_decode_mcu(j);
            jp_advance(j, d);
        }
        /* Out of buffered data (or past a restart): copy more. With final
         * input the next round decodes what is left or stops. */
        if (!j->scan_done && !*len && !final)
            return SCAN_NEED;
    }
truncated:
    /* Input ended inside the scan: finish the MCU row in progress. */
    if (!j->coefmode && j->mx > 0) {
        j->starved = true;
        while (j->mx > 0 && !j->scan_done) {
            jp_decode_mcu(j);
            jp_advance(j, d);
        }
    }
    return SCAN_TRUNC;
}

/* ---- Driver ---- */

static AuImageStatus jp_finish(Jp *j, AuImageDecoder *d)
{
    if (j->coefmode && j->began && j->nscans > 0) {
        jp_output_all(j, d);
        jp_free_coefs(j, d);
    } else if (j->began && j->rows_out) {
        au_img_frame_done(d, 0);
        j->state = S_DONE;
        return AU_IMAGE_FRAME;
    }
    return au_img_fail(d, "truncated");
}

static AuImageStatus jp_eoi(Jp *j, AuImageDecoder *d)
{
    if (!j->began)
        return au_img_fail(d, "corrupt JPEG: no image data");
    if (j->coefmode) {
        jp_output_all(j, d);
        jp_free_coefs(j, d);
    }
    au_img_frame_done(d, 0);
    j->state = S_DONE;
    return AU_IMAGE_FRAME;
}

static bool jp_wants(int m)
{
    return (m >= 0xC0 && m <= 0xCF) || m == 0xDA || m == 0xDB || m == 0xDD || m == 0xE1 || m == 0xEE;
}

static AuImageStatus jp_segment(Jp *j, AuImageDecoder *d, const uint8_t *p, size_t n)
{
    int m = j->marker;
    switch (m) {
    case 0xC4:
        if (!jp_parse_dht(j, p, n))
            return au_img_fail(d, "corrupt JPEG Huffman table");
        return AU_IMAGE_MORE;
    case 0xDB:
        if (!jp_parse_dqt(j, p, n))
            return au_img_fail(d, "corrupt JPEG quantization table");
        return AU_IMAGE_MORE;
    case 0xDD:
        if (n < 2)
            return au_img_fail(d, "corrupt JPEG restart interval");
        j->dri = (int)au_img_be16(p);
        return AU_IMAGE_MORE;
    case 0xE1:
        if (!j->have_sof)
            jp_parse_exif(j, p, n);
        return AU_IMAGE_MORE;
    case 0xEE:
        if (n >= 12 && !memcmp(p, "Adobe", 5) && !j->have_sof) {
            j->adobe = true;
            j->adobe_transform = p[11];
        }
        return AU_IMAGE_MORE;
    case 0xDA: {
        AuImageStatus st = jp_parse_sos(j, d, p, n);
        if (st == AU_IMAGE_MORE)
            j->state = S_SCAN;
        return st;
    }
    case 0xC0:
    case 0xC1:
    case 0xC2: {
        if (j->have_sof)
            return au_img_fail(d, "corrupt JPEG: two frames");
        j->progressive = m == 0xC2;
        AuImageStatus st = jp_parse_sof(j, d, p, n);
        if (st == AU_IMAGE_MORE && d->opt.header_only)
            return AU_IMAGE_DONE;
        return st;
    }
    case 0xC3:
    case 0xC7:
    case 0xCB:
    case 0xCF: return au_img_fail(d, "unsupported JPEG (lossless)");
    case 0xC5:
    case 0xC6: return au_img_fail(d, "unsupported JPEG (hierarchical)");
    case 0xC9:
    case 0xCA:
    case 0xCD:
    case 0xCE: return au_img_fail(d, "unsupported JPEG (arithmetic)");
    default: return AU_IMAGE_MORE; /* DAC, JPG: ignored */
    }
}

static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    Jp *j = st;
    for (;;) {
        switch (j->state) {
        case S_SOI:
            if (!au_img_hold(&j->hold, 2, in, len))
                goto more;
            if (j->hold.buf[0] != 0xFF || j->hold.buf[1] != 0xD8)
                return au_img_fail(d, "not a JPEG");
            j->hold.len = 0;
            j->state = S_MARKER;
            break;
        case S_MARKER: {
            int m = -1;
            while (*len) {
                uint8_t c = **in;
                (*in)++;
                (*len)--;
                if (!j->ff) {
                    j->ff = c == 0xFF; /* other bytes between segments are skipped */
                    continue;
                }
                if (c == 0xFF)
                    continue;
                j->ff = false;
                if (c == 0)
                    continue;
                m = c;
                break;
            }
            if (m < 0)
                goto more;
            j->marker = m;
        marker:
            m = j->marker;
            if (m == 0xD9) {
                return jp_eoi(j, d);
            } else if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                j->state = S_MARKER;
            } else {
                j->state = S_LEN;
            }
            break;
        }
        case S_LEN: {
            if (!au_img_hold(&j->hold, 2, in, len))
                goto more;
            size_t l = au_img_be16(j->hold.buf);
            j->hold.len = 0;
            if (l < 2)
                return au_img_fail(d, "corrupt JPEG marker length");
            j->seglen = l - 2;
            bool want = jp_wants(j->marker);
            if (j->marker == 0xE1 && (j->exif_seen || j->have_sof))
                want = false;
            j->state = want ? S_SEG : S_SKIP;
            break;
        }
        case S_SKIP: {
            size_t n = j->seglen < *len ? j->seglen : *len;
            *in += n;
            *len -= n;
            j->seglen -= n;
            if (j->seglen)
                goto more;
            j->state = S_MARKER;
            break;
        }
        case S_SEG: {
            if (!au_img_hold(&j->hold, j->seglen, in, len))
                goto more;
            j->hold.len = 0;
            j->state = S_MARKER;
            AuImageStatus r = jp_segment(j, d, j->hold.buf, j->seglen);
            if (r != AU_IMAGE_MORE)
                return r;
            if (j->hold.cap > 4096) {
                au_img_hold_free(&j->hold); /* do not keep a large APP1 */
            }
            break;
        }
        case S_SCAN: {
            int r = jp_scan(j, d, in, len, final);
            if (r == SCAN_MARKER) {
                j->state = S_MARKER;
                goto marker;
            }
            if (r == SCAN_TRUNC)
                return jp_finish(j, d);
            goto more;
        }
        default: return AU_IMAGE_DONE;
        }
    }
more:
    if (final)
        return jp_finish(j, d);
    return AU_IMAGE_MORE;
}

static void *create(AuImageDecoder *d)
{
    Jp *j = au_calloc(sizeof *j);
    j->orient = 1;
    j->mem = sizeof *j;
    au_img_mem(d, (ptrdiff_t)sizeof *j);
    return j;
}

static void destroy(void *st)
{
    Jp *j = st;
    for (int c = 0; c < 4; c++) {
        au_free(j->comp[c].sbuf);
        au_free(j->comp[c].up);
        au_free(j->comp[c].coef);
        au_free(j->comp[c].mask);
    }
    au_free(j->argb);
    au_free(j->eb);
    au_img_hold_free(&j->hold);
    au_free(j);
}

const AuImgCodec au_img_jpeg_codec = {create, run, destroy};
