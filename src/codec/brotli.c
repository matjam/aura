/* Aura brotli decoder (RFC 7932). See brotli.h.
 *
 * The decoder is a state machine over the stream's fields. Every state
 * either completes its read or consumes nothing, so input may stop at any
 * byte and resume. Bits come from a 32-bit accumulator filled a byte at a
 * time (least significant bit first, as the format stores them), and only
 * when a read needs the byte, so bytes after the end of the stream stay
 * in *in.
 *
 * Output goes to a ring buffer that holds the window (the history that
 * backward references reach) and is flushed to the caller's buffer. A
 * byte is never written over another that has not been flushed.
 *
 * Prefix codes decode through tables: a root table indexed by the next 8
 * bits, and second-level tables for longer codes. All tables of one tree
 * group (literals, insert-and-copy, distances) share one array. */
#include "brotli.h"

#include <string.h>

#include "brotli_int.h"

#define ROOT_BITS 8
#define MAX_CODE_LEN 15
#define MAX_ALPHABET 704
#define NUM_TYPES_MAX 256

typedef struct {
    uint16_t value; /* symbol, or the offset of a second-level table */
    uint8_t bits;   /* code length; above ROOT_BITS in a root entry: a
                       link to a table of (bits - ROOT_BITS) index bits */
    uint8_t pad;
} HEnt;

typedef struct {
    HEnt *t;
    uint32_t len, cap;
    uint32_t off[NUM_TYPES_MAX]; /* start of each tree in t */
    uint32_t n;
} HGroup;

typedef struct {
    uint32_t n;            /* number of block types */
    uint32_t type, last, second;
    uint32_t count;        /* symbols left in the current block */
    uint32_t type_tree, count_tree; /* index in the block tree group */
} BlkCat;

/* Reads one prefix code (RFC 7932 section 3.4 to 3.5). */
typedef struct {
    int stage;
    uint32_t alpha;
    HGroup *dst;
    uint32_t nsym, k;
    uint16_t syms[4];
    uint8_t cl_lens[18];
    uint32_t cl_i, cl_num;
    int32_t cl_space;
    HEnt cl_t[32];
    uint32_t sym, prev_len, rep_len, repeat;
    int32_t space;
    uint8_t lens[MAX_ALPHABET];
} PReader;

/* Reads one context map (section 7.3). */
typedef struct {
    int stage;
    uint8_t *map;
    uint32_t size, ntrees, i, rlemax, run_sym;
} MReader;

enum {
    ST_WBITS,
    ST_HDR_ISLAST,
    ST_HDR_ISLASTEMPTY,
    ST_HDR_MNIBBLES,
    ST_HDR_MLEN,
    ST_MD_RESERVED,
    ST_MD_SKIPLEN,
    ST_MD_ALIGN,
    ST_MD_SKIP,
    ST_HDR_ISUNCOMP,
    ST_UNC_ALIGN,
    ST_UNC_COPY,
    ST_HDR_NBLTYPES,
    ST_HDR_BTYPE_TREE,
    ST_HDR_BCOUNT_TREE,
    ST_HDR_BCOUNT,
    ST_HDR_DISTPARAMS,
    ST_HDR_CMODES,
    ST_HDR_NTREESL,
    ST_HDR_CMAPL,
    ST_HDR_NTREESD,
    ST_HDR_CMAPD,
    ST_HDR_TREES,
    ST_CMD_BEGIN,
    ST_CMD_INS_EXTRA,
    ST_CMD_COPY_EXTRA,
    ST_LITERALS,
    ST_DIST,
    ST_DIST_EXTRA,
    ST_COPY,
    ST_DICT_COPY,
    ST_MB_END,
    ST_DONE_PAD,
    ST_DONE,
    ST_ERROR
};

/* Results of the sub-readers and of decode(). */
enum { R_OK, R_NEED_IN, R_NEED_OUT, R_DONE, R_ERR };

struct AuBrotli {
    AuDecoder base; /* first, so the AuDecoder converts back */
    const uint8_t *dict;
    uint32_t max_wbits;
    int state;
    const char *err;

    const uint8_t *next_in;
    size_t avail_in;
    uint8_t *next_out;
    size_t avail_out;
    uint32_t val, nbits;

    uint32_t wbits, window;
    uint8_t *ring;
    uint32_t rsize, rmask, widx, unflushed;
    uint32_t hist; /* bytes of history, up to window */
    uint8_t p1, p2;

    int is_last;
    uint32_t mnibbles, mlen, remaining, skip;

    BlkCat blk[3];
    uint32_t hcat, htree;
    int bs_stage;
    uint32_t bs_code;
    uint32_t npostfix, ndirect, dist_alpha;
    uint32_t ntreesl, ntreesd;
    uint8_t cmodes[NUM_TYPES_MAX];
    uint8_t cmapl[64 * NUM_TYPES_MAX];
    uint8_t cmapd[4 * NUM_TYPES_MAX];
    HGroup gl, gi, gd, gb, gt;

    PReader pr;
    MReader mr;

    /* The current command. */
    uint32_t cmd, ins_len, copy_len, dist;
    uint32_t dist_rb[4], rbi;
    const uint8_t *lit_map, *lit_lut;
    const HEnt *cmd_tree;
    const uint8_t *dist_map;
    uint8_t word[AU_BROTLI_WORD_BUF];
    uint32_t word_len, word_pos;

    /* Per context mode: ctx = lut[p1] | lut[256 + p2]. */
    uint8_t ctx_lut[4][512];
};

/* ---- Constant tables (RFC 7932) ---- */

static const uint8_t cl_order[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
/* The fixed code for code length code lengths (section 3.5), indexed by
 * the next 4 bits. */
static const uint8_t cl_len[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
static const uint8_t cl_val[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};

static const uint16_t count_base[26] = {1,   5,   9,   13,  17,  25,   33,   41,   49,   65,   81,   97,    113,
                                        145, 177, 209, 241, 305, 369,  497,  753,  1265, 2289, 4337, 8433, 16625};
static const uint8_t count_extra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};

static const uint16_t ins_base[24] = {0,  1,  2,  3,  4,   5,   6,   8,   10,  14,   18,   26,
                                      34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t ins_extra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint16_t copy_base[24] = {2,  3,  4,  5,  6,   7,   8,   9,   10,  12,  14,   18,
                                       22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t copy_extra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
/* Insert-and-copy code: the 64-code cell gives the base insert and copy
 * length codes (section 5). Cells 0 and 1 use the last distance. */
static const uint8_t cell_ins[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
static const uint8_t cell_copy[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};

/* Static dictionary layout (section 8). */
static const uint8_t dict_ndbits[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10,
                                        9, 9, 8, 7, 7,  8,  7,  7,  6,  6,  5,  5};
static const uint32_t dict_offset[25] = {0,     0,     0,     0,     0,      4096,   9216,   21504,  35840,
                                         44032, 53248, 63488, 74752, 87040,  93696,  100864, 104704, 106752,
                                         108928, 113536, 115968, 118528, 119872, 121280, 122016};

/* ---- Errors ---- */

static int fail(AuBrotli *b, const char *msg)
{
    if (b->state != ST_ERROR) {
        b->err = msg;
        b->state = ST_ERROR;
    }
    return R_ERR;
}

/* ---- Bit reader ---- */

/* Bytes are taken only when a read needs bits from them, so after the
 * stream's last field no byte beyond it has been taken. */
static inline bool br_pull(AuBrotli *b)
{
    if (!b->avail_in)
        return false;
    b->val |= (uint32_t)*b->next_in++ << b->nbits;
    b->nbits += 8;
    b->avail_in--;
    return true;
}

/* n is at most 24, so the accumulator never holds more than 31 bits. */
static inline bool br_have(AuBrotli *b, uint32_t n)
{
    while (b->nbits < n)
        if (!br_pull(b))
            return false;
    return true;
}

static inline void br_drop(AuBrotli *b, uint32_t n)
{
    b->val >>= n;
    b->nbits -= n;
}

/* Reads n (0..24) bits, or nothing if the input runs out first. */
static inline bool br_read(AuBrotli *b, uint32_t n, uint32_t *v)
{
    if (!br_have(b, n))
        return false;
    *v = b->val & ((1u << n) - 1);
    br_drop(b, n);
    return true;
}

/* Drops the bits up to the next byte boundary; they must be zero. */
static bool br_align(AuBrotli *b)
{
    uint32_t r = b->nbits & 7;
    if (b->val & ((1u << r) - 1))
        return false;
    br_drop(b, r);
    return true;
}

/* Decodes one symbol. The table is read with the bits not yet taken
 * counted as zero (the accumulator holds zeros above nbits); the result
 * stands if the code found fits in the bits held, which the prefix
 * property makes exact. Otherwise one more byte is taken. */
static inline bool huff_read(AuBrotli *b, const HEnt *t, uint32_t *sym)
{
    for (;;) {
        uint32_t v = b->val, n;
        HEnt e = t[v & ((1u << ROOT_BITS) - 1)];
        n = e.bits;
        if (n > ROOT_BITS) {
            e = t[e.value + ((v >> ROOT_BITS) & ((1u << (n - ROOT_BITS)) - 1))];
            n = ROOT_BITS + e.bits;
        }
        if (n <= b->nbits) {
            br_drop(b, n);
            *sym = e.value;
            return true;
        }
        if (!br_pull(b))
            return false;
    }
}

/* ---- Prefix code tables ---- */

static uint32_t bit_reverse(uint32_t x, uint32_t n)
{
    uint32_t r = 0;
    while (n--) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/* Builds the decoding table for a complete prefix code with code lengths
 * lens[0..n) into t, or with t NULL only measures it. Returns the number
 * of entries. Canonical codes (section 3.2) are assigned in order of
 * length, then symbol; the stream sends each code's first bit first, so
 * table indexes are the codes bit-reversed. */
static uint32_t huff_build(HEnt *t, const uint8_t *lens, uint32_t n)
{
    uint32_t count[MAX_CODE_LEN + 1] = {0}, start[MAX_CODE_LEN + 2], rem[MAX_CODE_LEN + 1];
    uint16_t sorted[MAX_ALPHABET];
    uint32_t s, l, k, i, idx = 0, code = 0, size = 1u << ROOT_BITS;
    uint32_t sub_prefix = UINT32_MAX, sub_off = 0, sub_bits = 0;

    for (s = 0; s < n; s++)
        count[lens[s]]++;
    start[1] = 0;
    for (l = 1; l <= MAX_CODE_LEN; l++)
        start[l + 1] = start[l] + count[l];
    for (s = 0; s < n; s++)
        if (lens[s])
            sorted[start[lens[s]]++] = (uint16_t)s;
    memcpy(rem, count, sizeof rem);

    for (l = 1; l <= MAX_CODE_LEN; l++) {
        for (k = 0; k < count[l]; k++, code++) {
            s = sorted[idx++];
            if (l <= ROOT_BITS) {
                if (t) {
                    HEnt e = {(uint16_t)s, (uint8_t)l, 0};
                    for (i = bit_reverse(code, l); i < (1u << ROOT_BITS); i += 1u << l)
                        t[i] = e;
                }
            } else {
                uint32_t prefix = code >> (l - ROOT_BITS), sl = l - ROOT_BITS;
                if (prefix != sub_prefix) {
                    /* A new second-level table: as deep as the longest
                     * code that shares this prefix. Codes fill the
                     * subtree from the left in canonical order. */
                    uint32_t ln = l;
                    int32_t left = 1 << sl;
                    while (ln < MAX_CODE_LEN) {
                        left -= (int32_t)rem[ln];
                        if (left <= 0)
                            break;
                        ln++;
                        left <<= 1;
                    }
                    sub_prefix = prefix;
                    sub_off = size;
                    sub_bits = ln - ROOT_BITS;
                    size += 1u << sub_bits;
                    if (t) {
                        HEnt e = {(uint16_t)sub_off, (uint8_t)(ROOT_BITS + sub_bits), 0};
                        t[bit_reverse(prefix, ROOT_BITS)] = e;
                    }
                }
                if (t) {
                    HEnt e = {(uint16_t)s, (uint8_t)sl, 0};
                    for (i = bit_reverse(code & ((1u << sl) - 1), sl); i < (1u << sub_bits); i += 1u << sl)
                        t[sub_off + i] = e;
                }
            }
            rem[l]--;
        }
        code <<= 1;
    }
    return size;
}

static HEnt *group_reserve(HGroup *g, uint32_t size)
{
    if (g->len + size > g->cap) {
        uint32_t cap = g->cap ? g->cap * 2 : 1024;
        while (cap < g->len + size)
            cap *= 2;
        g->t = au_realloc(g->t, (size_t)cap * sizeof(HEnt));
        g->cap = cap;
    }
    g->off[g->n++] = g->len;
    g->len += size;
    return g->t + g->len - size;
}

/* Adds a code whose single symbol takes no bits. */
static void group_add_single(HGroup *g, uint32_t sym)
{
    HEnt *t = group_reserve(g, 1u << ROOT_BITS);
    HEnt e = {(uint16_t)sym, 0, 0};
    uint32_t i;
    for (i = 0; i < (1u << ROOT_BITS); i++)
        t[i] = e;
}

static void group_add(HGroup *g, const uint8_t *lens, uint32_t n)
{
    uint32_t size = huff_build(NULL, lens, n);
    HEnt *t = group_reserve(g, size);
    huff_build(t, lens, n);
}

static void group_reset(HGroup *g)
{
    g->len = 0;
    g->n = 0;
}

static inline const HEnt *group_tree(const HGroup *g, uint32_t i) { return g->t + g->off[i]; }

/* ---- Prefix code reader ---- */

enum { P_HSKIP, P_SIMPLE_N, P_SIMPLE_SYM, P_SIMPLE_SEL, P_CLCL, P_LENS };

static void pr_start(AuBrotli *b, uint32_t alpha, HGroup *dst)
{
    b->pr.stage = P_HSKIP;
    b->pr.alpha = alpha;
    b->pr.dst = dst;
}

static uint32_t alpha_bits(uint32_t alpha)
{
    uint32_t n = 0;
    while ((1u << n) < alpha)
        n++;
    return n;
}

static int pr_finish_simple(AuBrotli *b, uint32_t sel)
{
    PReader *p = &b->pr;
    if (p->nsym == 1) {
        group_add_single(p->dst, p->syms[0]);
        return R_OK;
    }
    memset(p->lens, 0, p->alpha);
    switch (p->nsym) {
    case 2:
        p->lens[p->syms[0]] = 1;
        p->lens[p->syms[1]] = 1;
        break;
    case 3:
        p->lens[p->syms[0]] = 1;
        p->lens[p->syms[1]] = 2;
        p->lens[p->syms[2]] = 2;
        break;
    default:
        if (sel) {
            p->lens[p->syms[0]] = 1;
            p->lens[p->syms[1]] = 2;
            p->lens[p->syms[2]] = 3;
            p->lens[p->syms[3]] = 3;
        } else {
            p->lens[p->syms[0]] = 2;
            p->lens[p->syms[1]] = 2;
            p->lens[p->syms[2]] = 2;
            p->lens[p->syms[3]] = 2;
        }
        break;
    }
    group_add(p->dst, p->lens, p->alpha);
    return R_OK;
}

static int pr_run(AuBrotli *b)
{
    PReader *p = &b->pr;
    uint32_t v, j;
    for (;;) {
        switch (p->stage) {
        case P_HSKIP:
            if (!br_read(b, 2, &v))
                return R_NEED_IN;
            if (v == 1) {
                p->stage = P_SIMPLE_N;
                break;
            }
            memset(p->cl_lens, 0, sizeof p->cl_lens);
            p->cl_i = v;
            p->cl_space = 32;
            p->cl_num = 0;
            p->stage = P_CLCL;
            break;
        case P_SIMPLE_N:
            if (!br_read(b, 2, &v))
                return R_NEED_IN;
            p->nsym = v + 1;
            p->k = 0;
            p->stage = P_SIMPLE_SYM;
            break;
        case P_SIMPLE_SYM: {
            uint32_t w = alpha_bits(p->alpha);
            while (p->k < p->nsym) {
                if (!br_read(b, w, &v))
                    return R_NEED_IN;
                if (v >= p->alpha)
                    return fail(b, "brotli: prefix code symbol out of range");
                for (j = 0; j < p->k; j++)
                    if (p->syms[j] == v)
                        return fail(b, "brotli: repeated symbol in a simple prefix code");
                p->syms[p->k++] = (uint16_t)v;
            }
            if (p->nsym == 4) {
                p->stage = P_SIMPLE_SEL;
                break;
            }
            return pr_finish_simple(b, 0);
        }
        case P_SIMPLE_SEL:
            if (!br_read(b, 1, &v))
                return R_NEED_IN;
            return pr_finish_simple(b, v);
        case P_CLCL: {
            uint32_t l, code, i, s;
            while (p->cl_i < 18) {
                uint32_t ix, n;
                for (;;) {
                    ix = b->val & 15;
                    n = cl_len[ix];
                    if (n <= b->nbits)
                        break;
                    if (!br_pull(b))
                        return R_NEED_IN;
                }
                br_drop(b, n);
                v = cl_val[ix];
                p->cl_lens[cl_order[p->cl_i++]] = (uint8_t)v;
                if (v) {
                    p->cl_space -= 32 >> v;
                    p->cl_num++;
                    if (p->cl_space <= 0)
                        break;
                }
            }
            if (p->cl_num != 1 && p->cl_space != 0)
                return fail(b, "brotli: invalid code length code");
            if (p->cl_num == 1) {
                for (s = 0; s < 18; s++)
                    if (p->cl_lens[s])
                        break;
                for (i = 0; i < 32; i++) {
                    p->cl_t[i].value = (uint16_t)s;
                    p->cl_t[i].bits = 0;
                }
            } else {
                code = 0;
                for (l = 1; l <= 5; l++) {
                    for (s = 0; s < 18; s++) {
                        if (p->cl_lens[s] != l)
                            continue;
                        for (i = bit_reverse(code, l); i < 32; i += 1u << l) {
                            p->cl_t[i].value = (uint16_t)s;
                            p->cl_t[i].bits = (uint8_t)l;
                        }
                        code++;
                    }
                    code <<= 1;
                }
            }
            memset(p->lens, 0, p->alpha);
            p->sym = 0;
            p->space = 32768;
            p->prev_len = 8;
            p->rep_len = 0;
            p->repeat = 0;
            p->stage = P_LENS;
            break;
        }
        case P_LENS:
            while (p->sym < p->alpha && p->space > 0) {
                HEnt e;
                uint32_t n, c;
                for (;;) {
                    e = p->cl_t[b->val & 31];
                    n = e.bits;
                    c = e.value;
                    if (n + (c == 16 ? 2 : c == 17 ? 3 : 0) <= b->nbits)
                        break;
                    if (!br_pull(b))
                        return R_NEED_IN;
                }
                if (c < 16) {
                    br_drop(b, n);
                    p->repeat = 0;
                    if (c) {
                        p->lens[p->sym] = (uint8_t)c;
                        p->prev_len = c;
                        p->space -= 32768 >> c;
                    }
                    p->sym++;
                } else {
                    uint32_t xb = c == 16 ? 2 : 3, extra, nl, old, delta;
                    extra = (b->val >> n) & ((1u << xb) - 1);
                    br_drop(b, n + xb);
                    nl = c == 16 ? p->prev_len : 0;
                    if (p->rep_len != nl) {
                        p->repeat = 0;
                        p->rep_len = nl;
                    }
                    old = p->repeat;
                    if (p->repeat > 0)
                        p->repeat = (p->repeat - 2) << xb;
                    p->repeat += extra + 3;
                    delta = p->repeat - old;
                    if (delta > p->alpha - p->sym)
                        return fail(b, "brotli: code lengths run past the alphabet");
                    if (nl) {
                        memset(p->lens + p->sym, (int)nl, delta);
                        p->space -= (int32_t)(delta * (32768u >> nl));
                    }
                    p->sym += delta;
                }
            }
            if (p->space != 0)
                return fail(b, "brotli: incomplete or oversubscribed prefix code");
            group_add(p->dst, p->lens, p->alpha);
            return R_OK;
        default:
            return fail(b, "brotli: internal error");
        }
    }
}

/* ---- Context map reader ---- */

enum { M_RLE, M_TREE, M_VAL, M_RUN, M_IMTF };

static void mr_start(AuBrotli *b, uint8_t *map, uint32_t size, uint32_t ntrees)
{
    b->mr.stage = M_RLE;
    b->mr.map = map;
    b->mr.size = size;
    b->mr.ntrees = ntrees;
}

static void inverse_mtf(uint8_t *map, uint32_t size)
{
    uint8_t mtf[256];
    uint32_t i;
    for (i = 0; i < 256; i++)
        mtf[i] = (uint8_t)i;
    for (i = 0; i < size; i++) {
        uint8_t idx = map[i], v = mtf[idx];
        map[i] = v;
        memmove(mtf + 1, mtf, idx);
        mtf[0] = v;
    }
}

static int mr_run(AuBrotli *b)
{
    MReader *m = &b->mr;
    uint32_t v, i;
    int r;
    for (;;) {
        switch (m->stage) {
        case M_RLE:
            if (!br_have(b, 1))
                return R_NEED_IN;
            if (!(b->val & 1)) {
                br_drop(b, 1);
                m->rlemax = 0;
            } else {
                if (!br_have(b, 5))
                    return R_NEED_IN;
                m->rlemax = ((b->val >> 1) & 15) + 1;
                br_drop(b, 5);
            }
            group_reset(&b->gt);
            pr_start(b, m->ntrees + m->rlemax, &b->gt);
            m->stage = M_TREE;
            break;
        case M_TREE:
            r = pr_run(b);
            if (r != R_OK)
                return r;
            m->i = 0;
            m->stage = M_VAL;
            break;
        case M_VAL: {
            const HEnt *t = group_tree(&b->gt, 0);
            while (m->i < m->size) {
                if (!huff_read(b, t, &v))
                    return R_NEED_IN;
                if (v == 0) {
                    m->map[m->i++] = 0;
                } else if (v <= m->rlemax) {
                    m->run_sym = v;
                    break;
                } else {
                    m->map[m->i++] = (uint8_t)(v - m->rlemax);
                }
            }
            m->stage = m->i < m->size ? M_RUN : M_IMTF;
            break;
        }
        case M_RUN: {
            uint32_t run;
            if (!br_read(b, m->run_sym, &v))
                return R_NEED_IN;
            run = (1u << m->run_sym) + v;
            if (run > m->size - m->i)
                return fail(b, "brotli: context map run too long");
            memset(m->map + m->i, 0, run);
            m->i += run;
            m->stage = M_VAL;
            break;
        }
        case M_IMTF:
            if (!br_read(b, 1, &v))
                return R_NEED_IN;
            if (v)
                inverse_mtf(m->map, m->size);
            for (i = 0; i < m->size; i++)
                if (m->map[i] >= m->ntrees)
                    return fail(b, "brotli: context map entry out of range");
            return R_OK;
        default:
            return fail(b, "brotli: internal error");
        }
    }
}

/* ---- Block switches (section 6) ---- */

/* Reads a block switch command for category cat, starting at the type
 * (bs_stage 0) or, for a meta-block's first block count, at the count
 * (bs_stage 1). */
static int read_block_switch(AuBrotli *b, uint32_t cat)
{
    BlkCat *c = &b->blk[cat];
    uint32_t s;
    switch (b->bs_stage) {
    case 0: {
        uint32_t type;
        if (!huff_read(b, group_tree(&b->gb, c->type_tree), &s))
            return R_NEED_IN;
        if (s == 0)
            type = c->second;
        else if (s == 1)
            type = c->last + 1 == c->n ? 0 : c->last + 1;
        else
            type = s - 2;
        c->second = c->last;
        c->last = c->type = type;
        b->bs_stage = 1;
    }
    /* fall through */
    case 1:
        if (!huff_read(b, group_tree(&b->gb, c->count_tree), &s))
            return R_NEED_IN;
        b->bs_code = s;
        b->bs_stage = 2;
    /* fall through */
    default:
        if (!br_read(b, count_extra[b->bs_code], &s))
            return R_NEED_IN;
        c->count = count_base[b->bs_code] + s;
        b->bs_stage = 0;
        return R_OK;
    }
}

static void set_lit_type(AuBrotli *b)
{
    uint32_t t = b->blk[0].type;
    b->lit_map = b->cmapl + 64 * t;
    b->lit_lut = b->ctx_lut[b->cmodes[t]];
}

/* ---- Output ---- */

static void flush(AuBrotli *b)
{
    while (b->unflushed && b->avail_out) {
        uint32_t start = (b->widx - b->unflushed) & b->rmask;
        size_t n = b->unflushed;
        if (n > b->rsize - start)
            n = b->rsize - start;
        if (n > b->avail_out)
            n = b->avail_out;
        memcpy(b->next_out, b->ring + start, n);
        b->next_out += n;
        b->avail_out -= n;
        b->unflushed -= (uint32_t)n;
    }
}

/* Free ring space, flushing first if there is none. */
static inline uint32_t ring_room(AuBrotli *b)
{
    if (b->unflushed == b->rsize)
        flush(b);
    return b->rsize - b->unflushed;
}

static inline void ring_advance(AuBrotli *b, uint32_t n)
{
    b->widx = (b->widx + n) & b->rmask;
    b->unflushed += n;
    b->hist = b->window - b->hist > n ? b->hist + n : b->window;
}

/* Refreshes the last two bytes after a copy, for literal contexts. */
static void update_p(AuBrotli *b)
{
    if (b->hist >= 1)
        b->p1 = b->ring[(b->widx - 1) & b->rmask];
    if (b->hist >= 2)
        b->p2 = b->ring[(b->widx - 2) & b->rmask];
}

/* Allocates the ring buffer before the first output. A stream whose
 * first data meta-block is also its last needs only that many bytes. */
static void ensure_ring(AuBrotli *b, bool last)
{
    uint32_t size;
    if (b->ring)
        return;
    size = b->window;
    if (last) {
        size = 64;
        while (size < b->mlen && size < b->window)
            size <<= 1;
    }
    b->ring = au_alloc(size);
    b->rsize = size;
    b->rmask = size - 1;
}

/* For an uncompressed meta-block (after alignment): true when the byte
 * after its data is in hand and holds an empty last meta-block. */
static bool peek_last_empty(AuBrotli *b)
{
    uint32_t held = b->nbits >> 3, c;
    if ((size_t)b->mlen >= held + b->avail_in)
        return false;
    if (b->mlen < held)
        c = (b->val >> (8 * b->mlen)) & 0xFF;
    else
        c = b->next_in[b->mlen - held];
    return (c & 3) == 3;
}

/* ---- Distances ---- */

/* Handles a decoded distance: a backward reference or a dictionary
 * word. push says whether the distance enters the ring of last
 * distances (distance code 0 never does). */
static int take_distance(AuBrotli *b, uint32_t dist, bool push)
{
    uint32_t max_dist = b->window - 16;
    if (max_dist > b->hist)
        max_dist = b->hist;
    if (dist > max_dist) {
        uint32_t len = b->copy_len, word_id, nd, idx, tr;
        if (len < 4 || len > 24)
            return fail(b, "brotli: invalid distance");
        if (!b->dict)
            return fail(b, "brotli: stream uses the static dictionary, which was not given");
        word_id = dist - max_dist - 1;
        nd = dict_ndbits[len];
        idx = word_id & ((1u << nd) - 1);
        tr = word_id >> nd;
        if (tr >= AU_BROTLI_NUM_TRANSFORMS)
            return fail(b, "brotli: invalid dictionary reference");
        b->word_len = au_brotli_transform(b->word, b->dict + dict_offset[len] + idx * len, len, tr);
        if (b->word_len > b->remaining)
            return fail(b, "brotli: dictionary word overruns the meta-block");
        b->remaining -= b->word_len;
        b->word_pos = 0;
        b->state = ST_DICT_COPY;
        return R_OK;
    }
    if (push) {
        b->dist_rb[b->rbi & 3] = dist;
        b->rbi++;
    }
    if (b->copy_len > b->remaining)
        return fail(b, "brotli: copy overruns the meta-block");
    b->remaining -= b->copy_len;
    b->dist = dist;
    b->state = ST_COPY;
    return R_OK;
}

static void after_copy(AuBrotli *b)
{
    update_p(b);
    b->state = b->remaining ? ST_CMD_BEGIN : ST_MB_END;
}

/* Decodes literals of the current command until they, the current block
 * or the free ring space run out. The hot state lives in locals: a store
 * into the ring may alias any field, so working on the struct would
 * reload every field after each byte. Returns false when the input runs
 * out (the state is then saved; the next call resumes). */
static bool decode_literals(AuBrotli *b)
{
    uint32_t val = b->val, nbits = b->nbits;
    const uint8_t *in = b->next_in;
    size_t avail = b->avail_in;
    uint8_t *ring = b->ring;
    uint32_t widx = b->widx, rmask = b->rmask;
    uint32_t n = b->ins_len, k;
    uint32_t p1 = b->p1, p2 = b->p2;
    const uint8_t *lut = b->lit_lut, *map = b->lit_map;
    const HEnt *lt = b->gl.t;
    const uint32_t *loff = b->gl.off;
    bool ok = true;

    if (n > b->blk[0].count)
        n = b->blk[0].count;
    if (n > b->rsize - b->unflushed)
        n = b->rsize - b->unflushed;
    for (k = 0; k < n; k++) {
        const HEnt *t = lt + loff[map[lut[p1] | lut[256 + p2]]];
        HEnt e;
        uint32_t len;
        for (;;) {
            e = t[val & ((1u << ROOT_BITS) - 1)];
            len = e.bits;
            if (len > ROOT_BITS) {
                e = t[e.value + ((val >> ROOT_BITS) & ((1u << (len - ROOT_BITS)) - 1))];
                len = ROOT_BITS + e.bits;
            }
            if (len <= nbits)
                break;
            if (!avail) {
                ok = false;
                goto out;
            }
            val |= (uint32_t)*in++ << nbits;
            nbits += 8;
            avail--;
        }
        val >>= len;
        nbits -= len;
        ring[widx] = (uint8_t)e.value;
        widx = (widx + 1) & rmask;
        p2 = p1;
        p1 = e.value;
    }
out:
    b->val = val;
    b->nbits = nbits;
    b->next_in = in;
    b->avail_in = avail;
    b->widx = widx;
    b->unflushed += k;
    b->hist = b->window - b->hist > k ? b->hist + k : b->window;
    b->p1 = (uint8_t)p1;
    b->p2 = (uint8_t)p2;
    b->blk[0].count -= k;
    b->ins_len -= k;
    return ok;
}

/* ---- The decoder ---- */

static void start_meta_block(AuBrotli *b)
{
    b->is_last = 0;
    group_reset(&b->gl);
    group_reset(&b->gi);
    group_reset(&b->gd);
    group_reset(&b->gb);
    b->state = ST_HDR_ISLAST;
}

static int decode(AuBrotli *b)
{
    uint32_t v;
    int r;
    for (;;) {
        switch (b->state) {
        case ST_WBITS:
            if (!br_have(b, 1))
                return R_NEED_IN;
            if (!(b->val & 1)) {
                br_drop(b, 1);
                b->wbits = 16;
            } else {
                if (!br_have(b, 4))
                    return R_NEED_IN;
                v = (b->val >> 1) & 7;
                if (v) {
                    br_drop(b, 4);
                    b->wbits = 17 + v;
                } else {
                    if (!br_have(b, 7))
                        return R_NEED_IN;
                    v = (b->val >> 4) & 7;
                    if (v == 1)
                        return fail(b, "brotli: reserved window size (large-window brotli is not supported)");
                    br_drop(b, 7);
                    b->wbits = v ? 8 + v : 17;
                }
            }
            if (b->wbits > b->max_wbits)
                return fail(b, "brotli: stream window (WBITS) is larger than the decoder allows");
            b->window = 1u << b->wbits;
            start_meta_block(b);
            break;

        case ST_HDR_ISLAST:
            if (!br_read(b, 1, &v))
                return R_NEED_IN;
            b->is_last = (int)v;
            b->state = v ? ST_HDR_ISLASTEMPTY : ST_HDR_MNIBBLES;
            break;
        case ST_HDR_ISLASTEMPTY:
            if (!br_read(b, 1, &v))
                return R_NEED_IN;
            b->state = v ? ST_DONE_PAD : ST_HDR_MNIBBLES;
            break;
        case ST_HDR_MNIBBLES:
            if (!br_read(b, 2, &v))
                return R_NEED_IN;
            if (v == 3) {
                /* Metadata; as the last meta-block it ends the stream. */
                b->state = ST_MD_RESERVED;
                break;
            }
            b->mnibbles = v + 4;
            b->state = ST_HDR_MLEN;
            break;
        case ST_HDR_MLEN:
            if (!br_read(b, 4 * b->mnibbles, &v))
                return R_NEED_IN;
            if (b->mnibbles > 4 && (v >> (4 * b->mnibbles - 4)) == 0)
                return fail(b, "brotli: meta-block length has a leading zero nibble");
            b->mlen = b->remaining = v + 1;
            b->state = b->is_last ? ST_HDR_NBLTYPES : ST_HDR_ISUNCOMP;
            b->hcat = 0;
            break;

        case ST_MD_RESERVED:
            if (!br_have(b, 1))
                return R_NEED_IN;
            if (b->val & 1)
                return fail(b, "brotli: reserved bit set");
            if (!br_read(b, 3, &v))
                return R_NEED_IN;
            b->mnibbles = v >> 1; /* MSKIPBYTES */
            b->skip = 0;
            b->state = b->mnibbles ? ST_MD_SKIPLEN : ST_MD_ALIGN;
            break;
        case ST_MD_SKIPLEN:
            if (!br_read(b, 8 * b->mnibbles, &v))
                return R_NEED_IN;
            if (b->mnibbles > 1 && (v >> (8 * b->mnibbles - 8)) == 0)
                return fail(b, "brotli: metadata length has a leading zero byte");
            b->skip = v + 1;
            b->state = ST_MD_ALIGN;
            break;
        case ST_MD_ALIGN:
            if (!br_align(b))
                return fail(b, "brotli: nonzero padding bits");
            b->state = ST_MD_SKIP;
            break;
        case ST_MD_SKIP:
            while (b->skip) {
                size_t n;
                if (b->nbits >= 8) {
                    br_drop(b, 8);
                    b->skip--;
                    continue;
                }
                if (!b->avail_in)
                    return R_NEED_IN;
                n = b->avail_in < b->skip ? b->avail_in : b->skip;
                b->next_in += n;
                b->avail_in -= n;
                b->skip -= (uint32_t)n;
            }
            b->state = ST_MB_END;
            break;

        case ST_HDR_ISUNCOMP:
            if (!br_read(b, 1, &v))
                return R_NEED_IN;
            b->state = v ? ST_UNC_ALIGN : ST_HDR_NBLTYPES;
            break;
        case ST_UNC_ALIGN:
            if (!br_align(b))
                return fail(b, "brotli: nonzero padding bits");
            ensure_ring(b, peek_last_empty(b));
            b->state = ST_UNC_COPY;
            break;
        case ST_UNC_COPY:
            while (b->remaining) {
                uint32_t room = ring_room(b), n;
                if (!room)
                    return R_NEED_OUT;
                if (b->nbits >= 8) {
                    b->ring[b->widx] = (uint8_t)b->val;
                    br_drop(b, 8);
                    ring_advance(b, 1);
                    b->remaining--;
                    continue;
                }
                if (!b->avail_in)
                    return R_NEED_IN;
                n = b->remaining;
                if (n > room)
                    n = room;
                if (n > b->rsize - b->widx)
                    n = b->rsize - b->widx;
                if (n > b->avail_in)
                    n = (uint32_t)b->avail_in;
                memcpy(b->ring + b->widx, b->next_in, n);
                b->next_in += n;
                b->avail_in -= n;
                ring_advance(b, n);
                b->remaining -= n;
            }
            update_p(b);
            b->state = ST_MB_END;
            break;

        case ST_HDR_NBLTYPES: {
            BlkCat *c = &b->blk[b->hcat];
            if (!br_have(b, 1))
                return R_NEED_IN;
            if (!(b->val & 1)) {
                br_drop(b, 1);
                v = 1;
            } else {
                uint32_t k;
                if (!br_have(b, 4))
                    return R_NEED_IN;
                k = (b->val >> 1) & 7;
                if (!k) {
                    br_drop(b, 4);
                    v = 2;
                } else {
                    if (!br_have(b, 4 + k))
                        return R_NEED_IN;
                    v = (1u << k) + ((b->val >> 4) & ((1u << k) - 1)) + 1;
                    br_drop(b, 4 + k);
                }
            }
            c->n = v;
            c->type = 0;
            c->last = 0;
            c->second = 1;
            if (v >= 2) {
                c->type_tree = b->gb.n;
                pr_start(b, v + 2, &b->gb);
                b->state = ST_HDR_BTYPE_TREE;
            } else {
                c->count = UINT32_MAX;
                if (++b->hcat == 3)
                    b->state = ST_HDR_DISTPARAMS;
            }
            break;
        }
        case ST_HDR_BTYPE_TREE:
            r = pr_run(b);
            if (r != R_OK)
                return r;
            b->blk[b->hcat].count_tree = b->gb.n;
            pr_start(b, 26, &b->gb);
            b->state = ST_HDR_BCOUNT_TREE;
            break;
        case ST_HDR_BCOUNT_TREE:
            r = pr_run(b);
            if (r != R_OK)
                return r;
            b->bs_stage = 1;
            b->state = ST_HDR_BCOUNT;
            break;
        case ST_HDR_BCOUNT:
            r = read_block_switch(b, b->hcat);
            if (r != R_OK)
                return r;
            b->state = ++b->hcat == 3 ? ST_HDR_DISTPARAMS : ST_HDR_NBLTYPES;
            break;

        case ST_HDR_DISTPARAMS:
            if (!br_read(b, 6, &v))
                return R_NEED_IN;
            b->npostfix = v & 3;
            b->ndirect = (v >> 2) << b->npostfix;
            b->dist_alpha = 16 + b->ndirect + (48u << b->npostfix);
            b->htree = 0;
            b->state = ST_HDR_CMODES;
            break;
        case ST_HDR_CMODES:
            while (b->htree < b->blk[0].n) {
                if (!br_read(b, 2, &v))
                    return R_NEED_IN;
                b->cmodes[b->htree++] = (uint8_t)v;
            }
            b->state = ST_HDR_NTREESL;
            break;
        case ST_HDR_NTREESL:
        case ST_HDR_NTREESD: {
            bool lit = b->state == ST_HDR_NTREESL;
            uint32_t size = lit ? 64 * b->blk[0].n : 4 * b->blk[2].n;
            uint8_t *map = lit ? b->cmapl : b->cmapd;
            if (!br_have(b, 1))
                return R_NEED_IN;
            if (!(b->val & 1)) {
                br_drop(b, 1);
                v = 1;
            } else {
                uint32_t k;
                if (!br_have(b, 4))
                    return R_NEED_IN;
                k = (b->val >> 1) & 7;
                if (!k) {
                    br_drop(b, 4);
                    v = 2;
                } else {
                    if (!br_have(b, 4 + k))
                        return R_NEED_IN;
                    v = (1u << k) + ((b->val >> 4) & ((1u << k) - 1)) + 1;
                    br_drop(b, 4 + k);
                }
            }
            if (lit)
                b->ntreesl = v;
            else
                b->ntreesd = v;
            if (v >= 2) {
                mr_start(b, map, size, v);
                b->state = lit ? ST_HDR_CMAPL : ST_HDR_CMAPD;
            } else {
                memset(map, 0, size);
                b->state = lit ? ST_HDR_NTREESD : ST_HDR_TREES;
            }
            b->hcat = 0;
            b->htree = 0;
            break;
        }
        case ST_HDR_CMAPL:
        case ST_HDR_CMAPD:
            r = mr_run(b);
            if (r != R_OK)
                return r;
            b->state = b->state == ST_HDR_CMAPL ? ST_HDR_NTREESD : ST_HDR_TREES;
            b->hcat = 0;
            b->htree = 0;
            break;
        case ST_HDR_TREES: {
            /* hcat: 0 literals, 1 insert-and-copy, 2 distances; htree
             * counts trees read, with a reader active when htree is odd. */
            static const uint32_t alpha_fixed[2] = {256, 704};
            HGroup *g = b->hcat == 0 ? &b->gl : b->hcat == 1 ? &b->gi : &b->gd;
            uint32_t want = b->hcat == 0 ? b->ntreesl : b->hcat == 1 ? b->blk[1].n : b->ntreesd;
            if (b->htree & 1) {
                r = pr_run(b);
                if (r != R_OK)
                    return r;
                b->htree++;
            }
            if (b->htree / 2 < want) {
                pr_start(b, b->hcat < 2 ? alpha_fixed[b->hcat] : b->dist_alpha, g);
                b->htree++;
                break;
            }
            b->htree = 0;
            if (++b->hcat < 3)
                break;
            /* The header is complete. */
            ensure_ring(b, b->is_last);
            set_lit_type(b);
            b->cmd_tree = group_tree(&b->gi, 0);
            b->dist_map = b->cmapd;
            b->bs_stage = 0;
            b->state = ST_CMD_BEGIN;
            break;
        }

        case ST_CMD_BEGIN: {
            uint32_t cell;
            if (b->blk[1].count == 0) {
                r = read_block_switch(b, 1);
                if (r != R_OK)
                    return r;
                b->cmd_tree = group_tree(&b->gi, b->blk[1].type);
            }
            if (!huff_read(b, b->cmd_tree, &v))
                return R_NEED_IN;
            b->blk[1].count--;
            b->cmd = v;
            cell = v >> 6;
            b->ins_len = cell_ins[cell] + ((v >> 3) & 7);  /* the code, for now */
            b->copy_len = cell_copy[cell] + (v & 7);
            b->state = ST_CMD_INS_EXTRA;
        }
        /* The states of a command follow each other in order. */
        /* fall through */
        case ST_CMD_INS_EXTRA: {
            uint32_t code = b->ins_len;
            if (!br_read(b, ins_extra[code], &v))
                return R_NEED_IN;
            b->ins_len = ins_base[code] + v;
            b->state = ST_CMD_COPY_EXTRA;
        }
        /* fall through */
        case ST_CMD_COPY_EXTRA: {
            uint32_t code = b->copy_len;
            if (!br_read(b, copy_extra[code], &v))
                return R_NEED_IN;
            b->copy_len = copy_base[code] + v;
            if (b->ins_len > b->remaining)
                return fail(b, "brotli: literals overrun the meta-block");
            b->remaining -= b->ins_len;
            b->state = ST_LITERALS;
        }
        /* fall through */
        case ST_LITERALS:
            while (b->ins_len) {
                if (b->blk[0].count == 0) {
                    r = read_block_switch(b, 0);
                    if (r != R_OK)
                        return r;
                    set_lit_type(b);
                }
                if (b->unflushed == b->rsize && !ring_room(b))
                    return R_NEED_OUT;
                if (!decode_literals(b))
                    return R_NEED_IN;
            }
            if (!b->remaining) {
                b->state = ST_MB_END;
                break;
            }
            b->state = ST_DIST;
        /* fall through */
        case ST_DIST: {
            uint32_t ctx, d;
            if (b->cmd < 128) {
                r = take_distance(b, b->dist_rb[(b->rbi - 1) & 3], false);
                if (r != R_OK)
                    return r;
                break;
            }
            if (b->blk[2].count == 0) {
                r = read_block_switch(b, 2);
                if (r != R_OK)
                    return r;
                b->dist_map = b->cmapd + 4 * b->blk[2].type;
            }
            ctx = b->copy_len > 4 ? 3 : b->copy_len - 2;
            if (!huff_read(b, group_tree(&b->gd, b->dist_map[ctx]), &v))
                return R_NEED_IN;
            b->blk[2].count--;
            if (v < 16) {
                static const int8_t delta[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
                static const uint8_t back[16] = {1, 2, 3, 4, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2};
                int64_t dd = (int64_t)b->dist_rb[(b->rbi - back[v]) & 3] + delta[v];
                if (dd <= 0)
                    return fail(b, "brotli: invalid distance");
                d = (uint32_t)dd;
                r = take_distance(b, d, v != 0);
                if (r != R_OK)
                    return r;
                break;
            }
            if (v < 16 + b->ndirect) {
                r = take_distance(b, v - 15, true);
                if (r != R_OK)
                    return r;
                break;
            }
            b->dist = v - b->ndirect - 16; /* the code, for now */
            b->state = ST_DIST_EXTRA;
            break;
        }
        case ST_DIST_EXTRA: {
            uint32_t code = b->dist, nbits = 1 + (code >> (b->npostfix + 1));
            uint32_t hcode = code >> b->npostfix, lcode = code & ((1u << b->npostfix) - 1);
            uint32_t offset, d;
            if (!br_read(b, nbits, &v))
                return R_NEED_IN;
            offset = ((2 + (hcode & 1)) << nbits) - 4;
            d = ((offset + v) << b->npostfix) + lcode + b->ndirect + 1;
            r = take_distance(b, d, true);
            if (r != R_OK)
                return r;
            break;
        }
        case ST_COPY:
            while (b->copy_len) {
                uint32_t room = ring_room(b), src, n;
                if (!room)
                    return R_NEED_OUT;
                src = (b->widx - b->dist) & b->rmask;
                n = b->copy_len;
                if (n > room)
                    n = room;
                if (n > b->rsize - b->widx)
                    n = b->rsize - b->widx;
                if (n > b->rsize - src)
                    n = b->rsize - src;
                if (n < 32 || (src < b->widx && b->widx - src < n)) {
                    /* Short, or overlapping: copy forward a byte at a
                     * time, so overlapping bytes repeat (as in a run).
                     * Forward order is also right when the source lies
                     * ahead of the destination. */
                    uint8_t *d = b->ring + b->widx;
                    const uint8_t *s = b->ring + src;
                    uint32_t i;
                    for (i = 0; i < n; i++)
                        d[i] = s[i];
                } else {
                    memmove(b->ring + b->widx, b->ring + src, n);
                }
                ring_advance(b, n);
                b->copy_len -= n;
            }
            after_copy(b);
            break;
        case ST_DICT_COPY:
            while (b->word_pos < b->word_len) {
                uint32_t room = ring_room(b), n;
                if (!room)
                    return R_NEED_OUT;
                n = b->word_len - b->word_pos;
                if (n > room)
                    n = room;
                if (n > b->rsize - b->widx)
                    n = b->rsize - b->widx;
                memcpy(b->ring + b->widx, b->word + b->word_pos, n);
                ring_advance(b, n);
                b->word_pos += n;
            }
            after_copy(b);
            break;

        case ST_MB_END:
            if (b->is_last)
                b->state = ST_DONE_PAD;
            else
                start_meta_block(b);
            break;
        case ST_DONE_PAD:
            if (!br_align(b))
                return fail(b, "brotli: nonzero padding bits at the end of the stream");
            b->state = ST_DONE;
            break;
        case ST_DONE:
            return R_DONE;
        case ST_ERROR:
        default:
            return R_ERR;
        }
    }
}

/* ---- Interface ---- */

AuBrotli *au_brotli_new(const AuBrotliDict *dict, int max_window_bits)
{
    AuBrotli *b = au_calloc(sizeof *b);
    uint32_t i;
    if (max_window_bits == 0)
        max_window_bits = AU_BROTLI_DEFAULT_MAX_WINDOW_BITS;
    if (max_window_bits < 10)
        max_window_bits = 10;
    if (max_window_bits > 24)
        max_window_bits = 24;
    b->max_wbits = (uint32_t)max_window_bits;
    b->dict = dict ? au_brotli_dict_bytes(dict) : NULL;
    b->err = "";
    b->state = ST_WBITS;
    /* Last distances, last first: 4, 11, 15, 16. */
    b->dist_rb[0] = 16;
    b->dist_rb[1] = 15;
    b->dist_rb[2] = 11;
    b->dist_rb[3] = 4;
    b->rbi = 4;
    for (i = 0; i < 256; i++) {
        b->ctx_lut[0][i] = (uint8_t)(i & 63);
        b->ctx_lut[1][i] = (uint8_t)(i >> 2);
        b->ctx_lut[2][i] = au_brotli_lut0[i];
        b->ctx_lut[2][256 + i] = au_brotli_lut1[i];
        b->ctx_lut[3][i] = (uint8_t)(au_brotli_lut2[i] << 3);
        b->ctx_lut[3][256 + i] = au_brotli_lut2[i];
    }
    return b;
}

void au_brotli_free(AuBrotli *b)
{
    if (!b)
        return;
    au_free(b->ring);
    au_free(b->gl.t);
    au_free(b->gi.t);
    au_free(b->gd.t);
    au_free(b->gb.t);
    au_free(b->gt.t);
    au_free(b);
}

AuCodecStatus au_brotli_run(AuBrotli *b, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len)
{
    int r;
    if (b->state == ST_ERROR)
        return AU_CODEC_ERROR;
    b->next_in = *in;
    b->avail_in = *in_len;
    b->next_out = *out;
    b->avail_out = *out_len;
    flush(b);
    r = decode(b);
    flush(b);
    *in = b->next_in;
    *in_len = b->avail_in;
    *out = b->next_out;
    *out_len = b->avail_out;
    if (r == R_ERR)
        return AU_CODEC_ERROR;
    if (r == R_DONE && b->unflushed == 0)
        return AU_CODEC_DONE;
    return AU_CODEC_OK;
}

const char *au_brotli_error(const AuBrotli *b) { return b->err; }

static AuCodecStatus dec_run(AuDecoder *d, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len)
{
    return au_brotli_run((AuBrotli *)d, in, in_len, out, out_len);
}
static void dec_free(AuDecoder *d) { au_brotli_free((AuBrotli *)d); }
static const char *dec_error(const AuDecoder *d) { return au_brotli_error((const AuBrotli *)d); }

AuDecoder *au_brotli_decoder(const AuBrotliDict *dict, int max_window_bits)
{
    AuBrotli *b = au_brotli_new(dict, max_window_bits);
    b->base.run = dec_run;
    b->base.free = dec_free;
    b->base.error = dec_error;
    return &b->base;
}
