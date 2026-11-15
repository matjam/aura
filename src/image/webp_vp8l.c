/* WebP lossless (VP8L) bitstream decoder.
 *
 * The coded image (after the colour-indexing width reduction) is decoded
 * whole into one ARGB array, since LZ77 back-references may reach any
 * earlier pixel. Every 16 completed rows the inverse transforms run on
 * those rows (into two small ping-pong buffers) and the rows are emitted,
 * so output appears as the stream decodes.
 *
 * Sub-images (transform data, the entropy image, the palette) are decoded
 * by the same code with level0 false; they cannot carry transforms or
 * meta codes, so nesting is at most one level deep.
 *
 * Huffman codes are two-level tables (8 root bits) built as libwebp
 * builds them: incomplete or over-subscribed codes are refused, except a
 * code with a single symbol, which reads no bits. */
#include "webp_int.h"

#define VL_ROOT 8
#define VL_LEN_ROOT 7
#define VL_NUM_LITERAL 256
#define VL_NUM_LENGTH 24
#define VL_NUM_DIST 40
#define VL_MAX_CACHE_BITS 11
#define VL_MAX_ALPHABET (VL_NUM_LITERAL + VL_NUM_LENGTH + (1 << VL_MAX_CACHE_BITS))
#define VL_ROWS 16

enum { XF_PREDICTOR, XF_CROSS_COLOR, XF_SUBTRACT_GREEN, XF_COLOR_INDEXING };

typedef struct {
    uint8_t bits;
    uint16_t value;
} VlCode;

/* ---- Bit reader (LSB first) ---- */

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    uint32_t val;
    int nbits;
    uint32_t over; /* zero bytes supplied past the end */
} VlBits;

static void lb_init(VlBits *b, const uint8_t *buf, size_t len)
{
    b->buf = buf;
    b->len = len;
    b->pos = 0;
    b->val = 0;
    b->nbits = 0;
    b->over = 0;
}

static inline void lb_fill(VlBits *b)
{
    while (b->nbits <= 24) {
        uint32_t c = 0;
        if (b->pos < b->len)
            c = b->buf[b->pos++];
        else if (b->over < 64)
            b->over++;
        b->val |= c << b->nbits;
        b->nbits += 8;
    }
}

/* More bits were consumed than the stream holds. */
static inline bool lb_eos(const VlBits *b) { return b->over && b->over * 8 > (uint32_t)b->nbits; }

/* n <= 24. */
static inline uint32_t lb_read(VlBits *b, int n)
{
    lb_fill(b);
    uint32_t v = b->val & ((1u << n) - 1);
    b->val >>= n;
    b->nbits -= n;
    return v;
}

static inline int lb_sym(VlBits *b, const VlCode *t)
{
    lb_fill(b);
    const VlCode *e = t + (b->val & ((1u << VL_ROOT) - 1));
    int nb = e->bits;
    if (nb > VL_ROOT) {
        b->val >>= VL_ROOT;
        b->nbits -= VL_ROOT;
        e += e->value + (b->val & ((1u << (nb - VL_ROOT)) - 1));
        nb = e->bits;
    }
    b->val >>= nb;
    b->nbits -= nb;
    return e->value;
}

/* The code-length code uses 7 root bits and never needs a second level
 * (its lengths are at most 7). */
static inline int lb_sym7(VlBits *b, const VlCode *t)
{
    lb_fill(b);
    const VlCode *e = t + (b->val & ((1u << VL_LEN_ROOT) - 1));
    b->val >>= e->bits;
    b->nbits -= e->bits;
    return e->value;
}

/* ---- Huffman tables ---- */

static void replicate(VlCode *t, int step, int end, VlCode c)
{
    do {
        end -= step;
        t[end] = c;
    } while (end > 0);
}

static uint32_t next_key(uint32_t key, int len)
{
    uint32_t step = 1u << (len - 1);
    while (key & step)
        step >>= 1;
    return step ? (key & (step - 1)) + step : key;
}

static int next_table_bits(const int *cnt, int len, int root)
{
    int left = 1 << (len - root);
    while (len < 15) {
        left -= cnt[len];
        if (left <= 0)
            break;
        ++len;
        left <<= 1;
    }
    return len - root;
}

/* Builds the table for code lengths lens[0..n) into root_table (NULL to
 * size it only). Returns the number of entries, 0 for an invalid code.
 * sorted is scratch for n symbols. */
static int huff_build(VlCode *root_table, int root_bits, const uint8_t *lens, int n, uint16_t *sorted)
{
    int count[16] = {0}, offset[16];
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15)
            return 0;
        count[lens[i]]++;
    }
    if (count[0] == n)
        return 0;
    offset[1] = 0;
    for (int len = 1; len < 15; len++)
        offset[len + 1] = offset[len] + count[len];
    for (int i = 0; i < n; i++)
        if (lens[i])
            sorted[offset[lens[i]]++] = (uint16_t)i;
    int total = 1 << root_bits;
    if (n - count[0] == 1) {
        if (root_table)
            for (int k = 0; k < total; k++)
                root_table[k] = (VlCode){0, sorted[0]};
        return total;
    }
    int cnt[16];
    memcpy(cnt, count, sizeof cnt);
    VlCode *table = root_table;
    int table_size = total, num_open = 1, sym = 0, len, step;
    uint32_t key = 0, low = 0xffffffffu, mask = (uint32_t)total - 1;
    for (len = 1, step = 2; len <= root_bits; ++len, step <<= 1) {
        num_open <<= 1;
        num_open -= cnt[len];
        if (num_open < 0)
            return 0;
        for (; cnt[len] > 0; --cnt[len]) {
            if (root_table)
                replicate(&table[key], step, table_size, (VlCode){(uint8_t)len, sorted[sym]});
            sym++;
            key = next_key(key, len);
        }
    }
    for (len = root_bits + 1, step = 2; len <= 15; ++len, step <<= 1) {
        num_open <<= 1;
        num_open -= cnt[len];
        if (num_open < 0)
            return 0;
        for (; cnt[len] > 0; --cnt[len]) {
            if ((key & mask) != low) {
                if (root_table)
                    table += table_size;
                int table_bits = next_table_bits(cnt, len, root_bits);
                table_size = 1 << table_bits;
                total += table_size;
                low = key & mask;
                if (root_table) {
                    root_table[low].bits = (uint8_t)(table_bits + root_bits);
                    root_table[low].value = (uint16_t)((table - root_table) - (ptrdiff_t)low);
                }
            }
            if (root_table)
                replicate(&table[key >> root_bits], step, table_size,
                          (VlCode){(uint8_t)(len - root_bits), sorted[sym]});
            sym++;
            key = next_key(key, len);
        }
    }
    return num_open == 0 ? total : 0;
}

/* ---- Decoder state ---- */

typedef struct {
    int type, bits, xsize; /* xsize: the width this transform outputs */
    uint32_t *data;        /* sub-image, or the palette (256 entries) */
} VlXform;

typedef struct {
    const VlCode *t[5];
} VlGroup;

typedef struct {
    AuImageDecoder *d;
    VlBits br;
    const char *err;
    int width, height;
    VlXform xf[4];
    int nxf;
    unsigned seen;
    uint8_t lens[VL_MAX_ALPHABET];
    uint16_t sorted[VL_MAX_ALPHABET];
    size_t accounted;

    /* Level-0 output. */
    AuWebpEmit emit;
    void *ctx;
    uint8_t *alpha; /* alpha mode: green channels go here */
    uint32_t *coded;
    int coded_w;
    int rows_done;
    uint32_t *buf[2];   /* ping-pong buffers, one leading row each */
    uint32_t *pred_prev; /* the predictor's last output row */
} VlDec;

/* One entropy-coded image's codes. */
typedef struct {
    int cache_bits;
    uint32_t *cache;
    int hbits, hxs;
    uint32_t *meta; /* group index per tile, or NULL */
    VlGroup *groups;
    int ngroups;
    VlCode *arena;
} VlStream;

static void vl_account(VlDec *v, ptrdiff_t n)
{
    v->accounted += (size_t)n;
    au_img_mem(v->d, n);
}

static bool vl_fail(VlDec *v, const char *msg)
{
    if (!v->err)
        v->err = msg;
    return false;
}

static void stream_free(VlDec *v, VlStream *s)
{
    au_free(s->cache);
    au_free(s->meta);
    au_free(s->groups);
    au_free(s->arena);
    (void)v;
    memset(s, 0, sizeof *s);
}

static inline int subsample(int size, int bits) { return (size + (1 << bits) - 1) >> bits; }

/* Allocates w * h pixels within the cap. NULL after failing. */
static uint32_t *vl_pixels(VlDec *v, int w, int h)
{
    size_t px, bytes;
    if (!au_img_mul((size_t)w, (size_t)h, &px) || !au_img_mul(px, 4, &bytes) || bytes > au_webp_pixel_cap(v->d)) {
        vl_fail(v, "lossless image too large");
        return NULL;
    }
    return au_alloc(bytes ? bytes : 4);
}

/* Reads one Huffman code of the given alphabet size into v->lens. */
static bool read_code_lengths(VlDec *v, int alphabet)
{
    VlBits *br = &v->br;
    uint8_t *lens = v->lens;
    memset(lens, 0, (size_t)alphabet);
    if (lb_read(br, 1)) { /* simple code */
        int nsym = (int)lb_read(br, 1) + 1;
        int first8 = (int)lb_read(br, 1);
        int s0 = (int)lb_read(br, first8 ? 8 : 1);
        if (s0 < alphabet)
            lens[s0] = 1;
        if (nsym == 2) {
            int s1 = (int)lb_read(br, 8);
            if (s1 < alphabet)
                lens[s1] = 1;
        }
        return !lb_eos(br) || vl_fail(v, "truncated");
    }
    static const uint8_t order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t cl[19] = {0};
    int ncl = (int)lb_read(br, 4) + 4;
    for (int i = 0; i < ncl; i++)
        cl[order[i]] = (uint8_t)lb_read(br, 3);
    VlCode lt[1 << VL_LEN_ROOT];
    if (!huff_build(lt, VL_LEN_ROOT, cl, 19, v->sorted))
        return vl_fail(v, "bad code length code");
    int max_symbol = alphabet;
    if (lb_read(br, 1)) {
        int nb = 2 + 2 * (int)lb_read(br, 3);
        max_symbol = 2 + (int)lb_read(br, nb);
        if (max_symbol > alphabet)
            return vl_fail(v, "bad code lengths");
    }
    int sym = 0, prev = 8;
    while (sym < alphabet) {
        if (max_symbol-- == 0)
            break;
        if (lb_eos(br))
            return vl_fail(v, "truncated");
        int c = lb_sym7(br, lt);
        if (c < 16) {
            lens[sym++] = (uint8_t)c;
            if (c)
                prev = c;
        } else {
            static const uint8_t extra[3] = {2, 3, 7}, off[3] = {3, 3, 11};
            int rep = (int)lb_read(br, extra[c - 16]) + off[c - 16];
            if (sym + rep > alphabet)
                return vl_fail(v, "bad code lengths");
            uint8_t l = c == 16 ? (uint8_t)prev : 0;
            while (rep-- > 0)
                lens[sym++] = l;
        }
    }
    return !lb_eos(br) || vl_fail(v, "truncated");
}

static bool decode_image(VlDec *v, int w, int h, bool level0, uint32_t **out);

/* Reads the codes of one image: the optional entropy image (level 0
 * only) and every group's five codes. */
static bool read_codes(VlDec *v, VlStream *s, int w, int h, bool level0)
{
    VlBits *br = &v->br;
    int ngroups_max = 1;
    int *map = NULL;
    int nmeta = 0;
    if (level0 && lb_read(br, 1)) {
        s->hbits = (int)lb_read(br, 3) + 2;
        s->hxs = subsample(w, s->hbits);
        int hys = subsample(h, s->hbits);
        if (!decode_image(v, s->hxs, hys, false, &s->meta))
            return false;
        nmeta = s->hxs * hys;
        for (int i = 0; i < nmeta; i++) {
            int g = (int)((s->meta[i] >> 8) & 0xffff);
            s->meta[i] = (uint32_t)g;
            if (g >= ngroups_max)
                ngroups_max = g + 1;
        }
        /* Store only the groups the entropy image uses: at most one per
         * tile, so a stream cannot make us keep 65536 unused groups. */
        map = au_alloc((size_t)ngroups_max * sizeof *map);
        for (int i = 0; i < ngroups_max; i++)
            map[i] = -1;
        int n = 0;
        for (int i = 0; i < nmeta; i++) {
            int g = (int)s->meta[i];
            if (map[g] < 0)
                map[g] = n++;
            s->meta[i] = (uint32_t)map[g];
        }
        s->ngroups = n;
    } else {
        s->ngroups = 1;
    }
    s->groups = au_calloc((size_t)s->ngroups * sizeof *s->groups);
    size_t (*offs)[5] = au_alloc((size_t)s->ngroups * sizeof *offs);
    size_t arena_n = 0, arena_cap = 0;
    bool ok = true;
    for (int i = 0; i < ngroups_max && ok; i++) {
        int gi = map ? map[i] : 0;
        for (int j = 0; j < 5; j++) {
            static const int alpha[5] = {VL_NUM_LITERAL + VL_NUM_LENGTH, 256, 256, 256, VL_NUM_DIST};
            int alphabet = alpha[j] + (j == 0 && s->cache_bits ? 1 << s->cache_bits : 0);
            if (!read_code_lengths(v, alphabet)) {
                ok = false;
                break;
            }
            int size = huff_build(NULL, VL_ROOT, v->lens, alphabet, v->sorted);
            if (!size) {
                ok = vl_fail(v, "bad Huffman code");
                break;
            }
            if (gi < 0)
                continue; /* unused group: validated, not kept */
            if (arena_n + (size_t)size > arena_cap) {
                arena_cap = (arena_n + (size_t)size) * 2;
                s->arena = au_realloc(s->arena, arena_cap * sizeof *s->arena);
            }
            huff_build(s->arena + arena_n, VL_ROOT, v->lens, alphabet, v->sorted);
            offs[gi][j] = arena_n;
            arena_n += (size_t)size;
        }
    }
    if (ok)
        for (int g = 0; g < s->ngroups; g++)
            for (int j = 0; j < 5; j++)
                s->groups[g].t[j] = s->arena + offs[g][j];
    au_free(offs);
    au_free(map);
    return ok;
}

static inline int copy_value(int sym, VlBits *br)
{
    if (sym < 4)
        return sym + 1;
    int extra = (sym - 2) >> 1;
    int offset = (2 + (sym & 1)) << extra;
    return offset + (int)lb_read(br, extra) + 1;
}

static const uint8_t code_to_plane[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b,
    0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d,
    0x68, 0x02, 0x67, 0x69, 0x12, 0x1e, 0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b,
    0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70,
};

static inline size_t plane_to_dist(int w, int code)
{
    if (code > 120)
        return (size_t)(code - 120);
    int dc = code_to_plane[code - 1];
    int dist = (dc >> 4) * w + (8 - (dc & 15));
    return dist >= 1 ? (size_t)dist : 1;
}

static void process_rows(VlDec *v, int upto);

/* Decodes the entropy-coded pixels of one image into data. */
static bool decode_pixels(VlDec *v, VlStream *s, uint32_t *data, int w, int h, bool level0)
{
    VlBits *br = &v->br;
    size_t total = (size_t)w * (size_t)h, pos = 0;
    int x = 0, y = 0;
    int hmask = s->meta ? (1 << s->hbits) - 1 : -1;
    int cshift = 32 - s->cache_bits;
    uint32_t *cache = s->cache;
    const VlGroup *g = &s->groups[0];
    while (pos < total) {
        if ((x & hmask) == 0)
            g = s->meta ? &s->groups[s->meta[(y >> s->hbits) * s->hxs + (x >> s->hbits)]] : &s->groups[0];
        if (lb_eos(br))
            return vl_fail(v, "truncated");
        int code = lb_sym(br, g->t[0]);
        if (code < VL_NUM_LITERAL) {
            uint32_t red = (uint32_t)lb_sym(br, g->t[1]);
            uint32_t blue = (uint32_t)lb_sym(br, g->t[2]);
            uint32_t alpha = (uint32_t)lb_sym(br, g->t[3]);
            uint32_t p = (alpha << 24) | (red << 16) | ((uint32_t)code << 8) | blue;
            data[pos++] = p;
            if (cache)
                cache[(0x1e35a7bdu * p) >> cshift] = p;
            if (++x >= w) {
                x = 0;
                ++y;
                if (level0 && y - v->rows_done >= VL_ROWS)
                    process_rows(v, y);
            }
        } else if (code < VL_NUM_LITERAL + VL_NUM_LENGTH) {
            size_t length = (size_t)copy_value(code - VL_NUM_LITERAL, br);
            int dsym = lb_sym(br, g->t[4]);
            size_t dist = plane_to_dist(w, copy_value(dsym, br));
            if (lb_eos(br))
                return vl_fail(v, "truncated");
            if (dist > pos || length > total - pos)
                return vl_fail(v, "bad backward reference");
            uint32_t *dst = data + pos;
            const uint32_t *src = dst - dist;
            for (size_t k = 0; k < length; k++)
                dst[k] = src[k];
            if (cache)
                for (size_t k = 0; k < length; k++)
                    cache[(0x1e35a7bdu * dst[k]) >> cshift] = dst[k];
            pos += length;
            /* Advance x, y without dividing in the common short case. */
            size_t nx = (size_t)x + length;
            if (nx >= (size_t)w) {
                y += (int)(nx / (size_t)w);
                nx %= (size_t)w;
                if (level0 && y - v->rows_done >= VL_ROWS)
                    process_rows(v, y);
            }
            x = (int)nx;
            if ((x & hmask) && pos < total && s->meta)
                g = &s->groups[s->meta[(y >> s->hbits) * s->hxs + (x >> s->hbits)]];
        } else {
            /* Colour cache index: only possible when a cache exists, as
             * the alphabet has no such symbols otherwise. */
            uint32_t p = cache[code - (VL_NUM_LITERAL + VL_NUM_LENGTH)];
            data[pos++] = p;
            cache[(0x1e35a7bdu * p) >> cshift] = p;
            if (++x >= w) {
                x = 0;
                ++y;
                if (level0 && y - v->rows_done >= VL_ROWS)
                    process_rows(v, y);
            }
        }
    }
    if (lb_eos(br))
        return vl_fail(v, "truncated");
    if (level0)
        process_rows(v, h);
    return true;
}

/* ---- Transforms ---- */

static inline uint32_t add_px(uint32_t a, uint32_t b)
{
    uint32_t ag = (a & 0xff00ff00u) + (b & 0xff00ff00u);
    uint32_t rb = (a & 0x00ff00ffu) + (b & 0x00ff00ffu);
    return (ag & 0xff00ff00u) | (rb & 0x00ff00ffu);
}

static inline uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }

static inline int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline int sub3(int a, int b, int c)
{
    int pb = b - c, pa = a - c;
    return (pb < 0 ? -pb : pb) - (pa < 0 ? -pa : pa);
}

static inline uint32_t select_px(uint32_t t, uint32_t l, uint32_t tl)
{
    int s = sub3((int)(t >> 24), (int)(l >> 24), (int)(tl >> 24)) +
            sub3((int)(t >> 16 & 255), (int)(l >> 16 & 255), (int)(tl >> 16 & 255)) +
            sub3((int)(t >> 8 & 255), (int)(l >> 8 & 255), (int)(tl >> 8 & 255)) +
            sub3((int)(t & 255), (int)(l & 255), (int)(tl & 255));
    return s <= 0 ? t : l;
}

static inline uint32_t clamp_full(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8)
        r |= (uint32_t)clip255((int)(a >> sh & 255) + (int)(b >> sh & 255) - (int)(c >> sh & 255)) << sh;
    return r;
}

static inline uint32_t clamp_half(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t ave = avg2(a, b), r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int x = (int)(ave >> sh & 255), y = (int)(c >> sh & 255);
        r |= (uint32_t)clip255(x + (x - y) / 2) << sh;
    }
    return r;
}

/* out[x] = in[x] + predictor(mode) over [x0, x1); up is the row above
 * out (up[x + 1] of the last pixel is out[0], the row's first pixel). */
static void predict_run(int mode, const uint32_t *in, const uint32_t *up, uint32_t *out, int x0, int x1)
{
    int x;
    switch (mode) {
    case 1:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], out[x - 1]);
        break;
    case 2:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], up[x]);
        break;
    case 3:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], up[x + 1]);
        break;
    case 4:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], up[x - 1]);
        break;
    case 5:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(avg2(out[x - 1], up[x + 1]), up[x]));
        break;
    case 6:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(out[x - 1], up[x - 1]));
        break;
    case 7:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(out[x - 1], up[x]));
        break;
    case 8:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(up[x - 1], up[x]));
        break;
    case 9:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(up[x], up[x + 1]));
        break;
    case 10:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], avg2(avg2(out[x - 1], up[x - 1]), avg2(up[x], up[x + 1])));
        break;
    case 11:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], select_px(up[x], out[x - 1], up[x - 1]));
        break;
    case 12:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], clamp_full(out[x - 1], up[x], up[x - 1]));
        break;
    case 13:
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], clamp_half(out[x - 1], up[x], up[x - 1]));
        break;
    default: /* 0, and 14 and 15 as libwebp treats them */
        for (x = x0; x < x1; x++)
            out[x] = add_px(in[x], 0xff000000u);
        break;
    }
}

static void inv_predictor(const VlXform *t, int y0, int y1, const uint32_t *in, uint32_t *out)
{
    int w = t->xsize;
    int y = y0;
    if (y == 0) {
        out[0] = add_px(in[0], 0xff000000u);
        for (int x = 1; x < w; x++)
            out[x] = add_px(in[x], out[x - 1]);
        in += w;
        out += w;
        y++;
    }
    int tw = 1 << t->bits, tiles = subsample(w, t->bits);
    for (; y < y1; y++, in += w, out += w) {
        const uint32_t *modes = t->data + (size_t)(y >> t->bits) * (size_t)tiles;
        const uint32_t *up = out - w;
        out[0] = add_px(in[0], up[0]);
        int x = 1;
        while (x < w) {
            int mode = (int)((*modes++ >> 8) & 15);
            int xe = (x & ~(tw - 1)) + tw;
            if (xe > w)
                xe = w;
            predict_run(mode, in, up, out, x, xe);
            x = xe;
        }
    }
}

static inline int ct_delta(int8_t pred, int8_t color) { return ((int)pred * color) >> 5; }

static void inv_cross_color(const VlXform *t, int y0, int y1, const uint32_t *in, uint32_t *out)
{
    int w = t->xsize, tw = 1 << t->bits, tiles = subsample(w, t->bits);
    for (int y = y0; y < y1; y++, in += w, out += w) {
        const uint32_t *m = t->data + (size_t)(y >> t->bits) * (size_t)tiles;
        for (int x0 = 0; x0 < w; x0 += tw, m++) {
            int8_t g2r = (int8_t)(*m & 255), g2b = (int8_t)(*m >> 8 & 255), r2b = (int8_t)(*m >> 16 & 255);
            int xe = x0 + tw < w ? x0 + tw : w;
            for (int x = x0; x < xe; x++) {
                uint32_t p = in[x];
                int8_t green = (int8_t)(p >> 8);
                int r = (int)(p >> 16 & 255), b = (int)(p & 255);
                r = (r + ct_delta(g2r, green)) & 255;
                b = (b + ct_delta(g2b, green) + ct_delta(r2b, (int8_t)r)) & 255;
                out[x] = (p & 0xff00ff00u) | ((uint32_t)r << 16) | (uint32_t)b;
            }
        }
    }
}

static void inv_subtract_green(const VlXform *t, int y0, int y1, const uint32_t *in, uint32_t *out)
{
    size_t n = (size_t)t->xsize * (size_t)(y1 - y0);
    for (size_t i = 0; i < n; i++) {
        uint32_t p = in[i], g = (p >> 8) & 255;
        uint32_t rb = ((p & 0x00ff00ffu) + ((g << 16) | g)) & 0x00ff00ffu;
        out[i] = (p & 0xff00ff00u) | rb;
    }
}

static void inv_color_indexing(const VlXform *t, int y0, int y1, const uint32_t *in, uint32_t *out)
{
    int w = t->xsize, iw = subsample(w, t->bits);
    const uint32_t *pal = t->data;
    if (t->bits == 0) {
        size_t n = (size_t)w * (size_t)(y1 - y0);
        for (size_t i = 0; i < n; i++)
            out[i] = pal[(in[i] >> 8) & 255];
        return;
    }
    int bpp = 8 >> t->bits, per = 1 << t->bits, cmask = per - 1;
    uint32_t bmask = (1u << bpp) - 1;
    for (int y = y0; y < y1; y++, in += iw, out += w) {
        uint32_t packed = 0;
        const uint32_t *src = in;
        for (int x = 0; x < w; x++) {
            if ((x & cmask) == 0)
                packed = (*src++ >> 8) & 255;
            out[x] = pal[packed & bmask];
            packed >>= bpp;
        }
    }
}

/* Applies the inverse transforms to completed rows and emits them. */
static void process_rows(VlDec *v, int upto)
{
    int w = v->width;
    while (v->rows_done < upto) {
        int y0 = v->rows_done, y1 = upto - y0 > VL_ROWS ? y0 + VL_ROWS : upto;
        const uint32_t *in = v->coded + (size_t)y0 * (size_t)v->coded_w;
        int which = 0;
        for (int i = v->nxf - 1; i >= 0; i--) {
            const VlXform *t = &v->xf[i];
            uint32_t *out = v->buf[which];
            which ^= 1;
            switch (t->type) {
            case XF_PREDICTOR:
                if (y0 > 0)
                    memcpy(out - t->xsize, v->pred_prev, (size_t)t->xsize * 4);
                inv_predictor(t, y0, y1, in, out);
                memcpy(v->pred_prev, out + (size_t)(y1 - y0 - 1) * (size_t)t->xsize, (size_t)t->xsize * 4);
                break;
            case XF_CROSS_COLOR: inv_cross_color(t, y0, y1, in, out); break;
            case XF_SUBTRACT_GREEN: inv_subtract_green(t, y0, y1, in, out); break;
            default: inv_color_indexing(t, y0, y1, in, out); break;
            }
            in = out;
        }
        uint32_t *rowbuf = v->buf[which];
        for (int y = y0; y < y1; y++) {
            const uint32_t *src = in + (size_t)(y - y0) * (size_t)w;
            if (v->alpha) {
                uint8_t *a = v->alpha + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++)
                    a[x] = (uint8_t)(src[x] >> 8);
            } else if (v->nxf) {
                v->emit(v->ctx, y, (uint32_t *)src); /* our own buffer */
            } else {
                /* The callback may change the row; never hand it the coded
                 * pixels, which later back-references read. */
                memcpy(rowbuf, src, (size_t)w * 4);
                v->emit(v->ctx, y, rowbuf);
            }
        }
        v->rows_done = y1;
    }
}

/* Reads one transform (level 0). w is the current coded width. */
static bool read_transform(VlDec *v, int *w)
{
    VlBits *br = &v->br;
    int type = (int)lb_read(br, 2);
    if (v->seen & (1u << type))
        return vl_fail(v, "repeated transform");
    v->seen |= 1u << type;
    VlXform *t = &v->xf[v->nxf++];
    t->type = type;
    t->xsize = *w;
    t->bits = 0;
    t->data = NULL;
    switch (type) {
    case XF_PREDICTOR:
    case XF_CROSS_COLOR:
        t->bits = (int)lb_read(br, 3) + 2;
        return decode_image(v, subsample(t->xsize, t->bits), subsample(v->height, t->bits), false, &t->data);
    case XF_COLOR_INDEXING: {
        int ncolors = (int)lb_read(br, 8) + 1;
        t->bits = ncolors > 16 ? 0 : ncolors > 4 ? 1 : ncolors > 2 ? 2 : 3;
        *w = subsample(t->xsize, t->bits);
        uint32_t *colors;
        if (!decode_image(v, ncolors, 1, false, &colors))
            return false;
        /* Delta-coded palette, padded with transparent black. */
        uint32_t *pal = au_calloc(256 * sizeof *pal);
        pal[0] = colors[0];
        for (int i = 1; i < ncolors; i++)
            pal[i] = add_px(colors[i], pal[i - 1]);
        au_free(colors);
        t->data = pal;
        return true;
    }
    default: return true;
    }
}

/* Decodes one image stream. level0: the main image (transforms, meta
 * codes, rows emitted through process_rows); otherwise a sub-image whose
 * pixels are returned in *out. */
static bool decode_image(VlDec *v, int w, int h, bool level0, uint32_t **out)
{
    VlBits *br = &v->br;
    VlStream s;
    memset(&s, 0, sizeof s);
    int cw = w;
    bool ok = true;
    if (level0) {
        while (ok && lb_read(br, 1)) {
            if (v->nxf >= 4)
                return vl_fail(v, "too many transforms");
            ok = read_transform(v, &cw);
        }
        if (!ok)
            return false;
    }
    if (lb_read(br, 1)) {
        s.cache_bits = (int)lb_read(br, 4);
        if (s.cache_bits < 1 || s.cache_bits > VL_MAX_CACHE_BITS)
            return vl_fail(v, "bad colour cache size");
    }
    if (lb_eos(br))
        return vl_fail(v, "truncated");
    if (!read_codes(v, &s, cw, h, level0)) {
        stream_free(v, &s);
        return false;
    }
    if (s.cache_bits)
        s.cache = au_calloc(((size_t)1 << s.cache_bits) * sizeof *s.cache);
    uint32_t *data = vl_pixels(v, cw, h);
    if (!data) {
        stream_free(v, &s);
        return false;
    }
    size_t bytes = (size_t)cw * (size_t)h * 4;
    vl_account(v, (ptrdiff_t)bytes);
    if (level0) {
        v->coded = data;
        v->coded_w = cw;
        v->rows_done = 0;
        /* Output buffers: the widest stage is the final width. */
        size_t rowpx = (size_t)w;
        for (int i = 0; i < 2; i++)
            v->buf[i] = (uint32_t *)au_alloc((VL_ROWS + 1) * rowpx * 4) + rowpx;
        v->pred_prev = au_alloc(rowpx * 4);
        vl_account(v, (ptrdiff_t)((2 * VL_ROWS + 3) * rowpx * 4));
    }
    ok = decode_pixels(v, &s, data, cw, h, level0);
    stream_free(v, &s);
    if (level0) {
        /* The pixels are freed by vl_cleanup. */
        return ok;
    }
    vl_account(v, -(ptrdiff_t)bytes);
    if (!ok) {
        au_free(data);
        return false;
    }
    *out = data;
    return true;
}

static void vl_cleanup(VlDec *v)
{
    for (int i = 0; i < v->nxf; i++)
        au_free(v->xf[i].data);
    au_free(v->coded);
    if (v->buf[0])
        au_free(v->buf[0] - v->width);
    if (v->buf[1])
        au_free(v->buf[1] - v->width);
    au_free(v->pred_prev);
    au_img_mem(v->d, -(ptrdiff_t)v->accounted);
    au_free(v);
}

static const char *vl_run(AuImageDecoder *d, const uint8_t *data, size_t len, int w, int h, bool header,
                          AuWebpEmit emit, void *ctx, uint8_t *alpha)
{
    VlDec *v = au_calloc(sizeof *v);
    v->d = d;
    lb_init(&v->br, data, len);
    if (header) {
        if (lb_read(&v->br, 8) != 0x2f) {
            au_free(v);
            return "bad lossless signature";
        }
        w = (int)lb_read(&v->br, 14) + 1;
        h = (int)lb_read(&v->br, 14) + 1;
        lb_read(&v->br, 1);
        if (lb_read(&v->br, 3) != 0) {
            au_free(v);
            return "bad lossless version";
        }
    }
    v->width = w;
    v->height = h;
    v->emit = emit;
    v->ctx = ctx;
    v->alpha = alpha;
    uint32_t *unused = NULL;
    bool ok = decode_image(v, w, h, true, &unused);
    const char *err = ok ? NULL : (v->err ? v->err : "corrupt lossless image");
    vl_cleanup(v);
    return err;
}

bool au_webp_vp8l_size(const uint8_t *p, size_t n, int *w, int *h, bool *alpha)
{
    if (n < 5 || p[0] != 0x2f)
        return false;
    uint32_t bits = au_img_le32(p + 1);
    if (bits >> 29)
        return false; /* version must be 0 */
    *w = (int)(bits & 0x3fff) + 1;
    *h = (int)((bits >> 14) & 0x3fff) + 1;
    *alpha = (bits >> 28) & 1;
    return true;
}

const char *au_webp_vp8l_decode(AuImageDecoder *d, const uint8_t *data, size_t len, AuWebpEmit emit, void *ctx)
{
    return vl_run(d, data, len, 0, 0, true, emit, ctx, NULL);
}

const char *au_webp_vp8l_decode_alpha(AuImageDecoder *d, const uint8_t *data, size_t len, int w, int h,
                                      uint8_t *alpha)
{
    return vl_run(d, data, len, w, h, false, NULL, NULL, alpha);
}
