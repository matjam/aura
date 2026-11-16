/* Deflate (RFC 1951) encoder with zlib (RFC 1950) and gzip (RFC 1952)
 * framing. See codec.h for the interface.
 *
 * The design is the classical one: a 64 KB buffer holds a 32 KB window of
 * history and the bytes ahead; three-byte strings are hashed into chains
 * (head, prev) that the matcher walks, newest first, for the longest
 * earlier occurrence; literals and matches collect in a symbol buffer;
 * and when the buffer is full (or the input ends) a block is written
 * with whichever of stored, fixed Huffman and dynamic Huffman coding is
 * smallest.
 *
 * Level 1 takes the first match found at each position (greedy, short
 * chains). Level 2 also looks one byte ahead for a longer match before
 * committing (lazy matching, longer chains). Level 0 stores.
 *
 * The encoder reads only its own buffers: indexes into the window come
 * from the hash chains, which hold positions the encoder wrote itself,
 * and every match length is capped by the bytes that are there. Nothing
 * in the input can make it fail; it only decides how well it compresses.
 *
 * Output goes to a pending buffer first (a block at most), and from
 * there to the caller's output as it has room, so a block is never
 * interrupted halfway. */
#include "codec.h"

#include <string.h>

#define WBITS 15
#define WSIZE (1u << WBITS)
#define WMASK (WSIZE - 1)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)
#define MAX_DIST (WSIZE - MIN_LOOKAHEAD)
#define HASH_BITS 15
#define HASH_SIZE (1u << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)
#define WIN_SLACK (MAX_MATCH + 8) /* the matcher's first comparisons may read past the data, never past this */
#define NIL 0

#define SYM_MAX 16384 /* literals and matches per block */
#define L_CODES 286
#define D_CODES 30
#define BL_CODES 19
#define END_BLOCK 256
#define MAX_BITS 15
#define MAX_BL_BITS 7
#define TOO_FAR 4096 /* a match of the minimum length this far back costs more than literals */

static const uint8_t extra_lbits[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t base_length[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t extra_dbits[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint16_t base_dist[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t bl_order[BL_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

typedef struct Code {
    uint16_t code; /* bit-reversed, ready to send low bit first */
    uint8_t len;
} Code;

typedef enum { ST_HEADER, ST_BODY, ST_TRAILER, ST_DONE } Stage;

struct AuDeflate {
    AuDeflateFormat fmt;
    int level;
    Stage stage;
    /* parameters of the level */
    uint32_t max_chain, nice_len, good_len, max_lazy;
    bool lazy;
    /* window and hash chains */
    uint8_t *win;   /* 2 * WSIZE + WIN_SLACK */
    uint16_t *head; /* HASH_SIZE: the last position with each hash, NIL for none */
    uint16_t *prev; /* WSIZE: the position before, with the same hash */
    uint32_t strstart, lookahead;
    long block_start; /* where the current block's bytes begin in win; negative once they slid out */
    uint32_t match_start, prev_length, prev_match;
    bool match_available;
    /* the block being collected */
    uint16_t *d_buf; /* SYM_MAX: distance, 0 for a literal */
    uint8_t *l_buf;  /* SYM_MAX: the literal, or the length - 3 */
    uint32_t nsyms;
    uint32_t lfreq[L_CODES + 2], dfreq[D_CODES], blfreq[BL_CODES];
    uint32_t block_bytes; /* input bytes the block covers */
    /* code tables */
    uint8_t len_code[256];  /* length - 3 to length code (0..28) */
    uint8_t dist_code[512]; /* distance - 1 (under 256), or 256 + ((distance - 1) >> 7) */
    Code fixed_l[L_CODES + 2], fixed_d[D_CODES];
    Code dyn_l[L_CODES + 2], dyn_d[D_CODES], bl[BL_CODES];
    /* output */
    AuBuf pend;
    size_t pend_pos;
    uint32_t bi_buf;
    int bi_valid;
    /* checksums and totals */
    uint32_t check;
    uint32_t total_in;
};

/* ---- Bit output ---- */

static void put_byte(AuDeflate *z, unsigned b)
{
    au_buf_putc(&z->pend, (char)b);
}

static void put_bits(AuDeflate *z, unsigned value, int n)
{
    z->bi_buf |= (uint32_t)value << z->bi_valid;
    z->bi_valid += n;
    while (z->bi_valid >= 8) {
        put_byte(z, z->bi_buf & 0xFF);
        z->bi_buf >>= 8;
        z->bi_valid -= 8;
    }
}

static void align_bits(AuDeflate *z)
{
    if (z->bi_valid > 0)
        put_byte(z, z->bi_buf & 0xFF);
    z->bi_buf = 0;
    z->bi_valid = 0;
}

static void send(AuDeflate *z, const Code *c)
{
    put_bits(z, c->code, c->len);
}

/* ---- Huffman codes ---- */

/* Canonical codes for the lengths in c[0..n). */
static void gen_codes(Code *c, int n)
{
    uint16_t count[MAX_BITS + 1], next[MAX_BITS + 1];
    memset(count, 0, sizeof count);
    for (int i = 0; i < n; i++)
        count[c[i].len]++;
    count[0] = 0;
    unsigned code = 0;
    for (int b = 1; b <= MAX_BITS; b++) {
        code = (code + count[b - 1]) << 1;
        next[b] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        int len = c[i].len;
        if (!len) {
            c[i].code = 0;
            continue;
        }
        unsigned v = next[len]++, r = 0;
        for (int k = 0; k < len; k++) {
            r = (r << 1) | (v & 1);
            v >>= 1;
        }
        c[i].code = (uint16_t)r;
    }
}

/* Code lengths for n symbols with frequencies freq, none longer than
 * max_bits: a Huffman tree built from two sorted queues; when it comes
 * out too deep the frequencies are halved (rounding up) and the tree
 * built again, which flattens it until it fits. At least two symbols get
 * a code, so the code is complete (decoders reject a lone one-bit code
 * in places). Returns the highest symbol with a code. */
static int build_lengths(const uint32_t *freq, int n, int max_bits, Code *out)
{
    uint32_t f[L_CODES + 2];
    int max_code = -1, used = 0;
    for (int i = 0; i < n; i++) {
        f[i] = freq[i];
        if (f[i]) {
            used++;
            max_code = i;
        }
    }
    /* Fewer than two symbols: lend a count to the first unused ones. */
    for (int i = 0; used < 2 && i < n; i++) {
        if (!f[i]) {
            f[i] = 1;
            used++;
            if (i > max_code)
                max_code = i;
        }
    }
    for (;;) {
        /* Leaves sorted by frequency (then by symbol). */
        uint16_t leaf[L_CODES + 2];
        int nl = 0;
        for (int i = 0; i < n; i++)
            if (f[i])
                leaf[nl++] = (uint16_t)i;
        for (int gap = nl / 2; gap > 0; gap /= 2) {
            for (int i = gap; i < nl; i++) {
                uint16_t v = leaf[i];
                int j = i;
                while (j >= gap && f[leaf[j - gap]] > f[v]) {
                    leaf[j] = leaf[j - gap];
                    j -= gap;
                }
                leaf[j] = v;
            }
        }
        if (nl < 2) {
            /* Not reached with an alphabet of two symbols or more (two
             * were given counts above); for the compiler's sake. */
            for (int i = 0; i < n; i++)
                out[i].len = f[i] ? 1 : 0;
            break;
        }
        /* Nodes 0..nl-1 are the leaves in that order; nl.. are internal,
         * made in order of weight, so they form a second sorted queue. */
        uint32_t w[2 * (L_CODES + 2)];
        uint16_t parent[2 * (L_CODES + 2)];
        for (int i = 0; i < nl; i++)
            w[i] = f[leaf[i]];
        int li = 0, ii = nl, next = nl;
        while (next < 2 * nl - 1) {
            int pick[2];
            for (int k = 0; k < 2; k++) {
                if (li < nl && (ii >= next || w[li] <= w[ii]))
                    pick[k] = li++;
                else
                    pick[k] = ii++;
            }
            w[next] = w[pick[0]] + w[pick[1]];
            parent[pick[0]] = parent[pick[1]] = (uint16_t)next;
            next++;
        }
        /* Depths, from the root down: an internal node's parent has a
         * higher index. */
        uint8_t depth[2 * (L_CODES + 2)];
        int root = 2 * nl - 2, deepest = 0;
        depth[root] = 0;
        for (int i = root - 1; i >= 0; i--) {
            depth[i] = (uint8_t)(depth[parent[i]] + 1);
            if (i < nl && depth[i] > deepest)
                deepest = depth[i];
        }
        if (deepest <= max_bits) {
            for (int i = 0; i < n; i++)
                out[i].len = 0;
            for (int i = 0; i < nl; i++)
                out[leaf[i]].len = depth[i];
            break;
        }
        for (int i = 0; i < n; i++)
            if (f[i])
                f[i] = (f[i] + 1) / 2;
    }
    gen_codes(out, n);
    return max_code;
}

static unsigned dist_code_of(const AuDeflate *z, unsigned dist)
{
    dist--;
    return dist < 256 ? z->dist_code[dist] : z->dist_code[256 + (dist >> 7)];
}

static void init_tables(AuDeflate *z)
{
    for (int c = 0; c < 29; c++) {
        unsigned hi = c == 28 ? 258 : (unsigned)base_length[c] + (1u << extra_lbits[c]) - 1;
        for (unsigned l = base_length[c]; l <= hi && l <= 258; l++)
            z->len_code[l - 3] = (uint8_t)c;
    }
    z->len_code[255] = 28; /* 258 has a code of its own */
    for (int c = 0; c < 30; c++) {
        unsigned lo = base_dist[c], hi = lo + (1u << extra_dbits[c]) - 1;
        for (unsigned d = lo; d <= hi; d++) {
            if (d - 1 < 256)
                z->dist_code[d - 1] = (uint8_t)c;
            else
                z->dist_code[256 + ((d - 1) >> 7)] = (uint8_t)c; /* codes 16 up cover whole groups of 128 */
        }
    }
    for (int i = 0; i < L_CODES + 2; i++)
        z->fixed_l[i].len = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    gen_codes(z->fixed_l, L_CODES + 2);
    for (int i = 0; i < D_CODES; i++)
        z->fixed_d[i].len = 5;
    gen_codes(z->fixed_d, D_CODES);
}

/* ---- Blocks ---- */

static void block_reset(AuDeflate *z)
{
    memset(z->lfreq, 0, sizeof z->lfreq);
    memset(z->dfreq, 0, sizeof z->dfreq);
    z->nsyms = 0;
    z->block_bytes = 0;
}

/* Records a literal (dist 0) or a match; true when the block is full. */
static bool tally(AuDeflate *z, unsigned dist, unsigned lc)
{
    z->d_buf[z->nsyms] = (uint16_t)dist;
    z->l_buf[z->nsyms] = (uint8_t)lc;
    z->nsyms++;
    if (!dist) {
        z->lfreq[lc]++;
        z->block_bytes++;
    } else {
        z->lfreq[257 + z->len_code[lc]]++;
        z->dfreq[dist_code_of(z, dist)]++;
        z->block_bytes += lc + MIN_MATCH;
    }
    return z->nsyms == SYM_MAX - 1;
}

/* The lengths of n codes as the code-length alphabet sends them: counts
 * the symbols into blfreq (emit false) or writes them (emit true). */
static void scan_lengths(AuDeflate *z, const Code *c, int n, bool emit)
{
    int i = 0;
    while (i < n) {
        int len = c[i].len, run = 1;
        while (i + run < n && c[i + run].len == len)
            run++;
        i += run;
        while (run > 0) {
            int sym, extra = 0, ebits = 0, take;
            if (len == 0 && run >= 11) {
                take = run > 138 ? 138 : run;
                sym = 18;
                extra = take - 11;
                ebits = 7;
            } else if (len == 0 && run >= 3) {
                take = run;
                sym = 17;
                extra = take - 3;
                ebits = 3;
            } else {
                /* The length itself; then, for a nonzero length, repeats of it. */
                take = 1;
                sym = len;
            }
            if (emit) {
                send(z, &z->bl[sym]);
                if (ebits)
                    put_bits(z, (unsigned)extra, ebits);
            } else {
                z->blfreq[sym]++;
            }
            run -= take;
            while (len != 0 && run >= 3) {
                take = run > 6 ? 6 : run;
                if (emit) {
                    send(z, &z->bl[16]);
                    put_bits(z, (unsigned)(take - 3), 2);
                } else {
                    z->blfreq[16]++;
                }
                run -= take;
            }
        }
    }
}

static void send_symbols(AuDeflate *z, const Code *lc, const Code *dc)
{
    for (uint32_t i = 0; i < z->nsyms; i++) {
        unsigned dist = z->d_buf[i], l = z->l_buf[i];
        if (!dist) {
            send(z, &lc[l]);
            continue;
        }
        unsigned code = z->len_code[l];
        send(z, &lc[257 + code]);
        if (extra_lbits[code])
            put_bits(z, l + MIN_MATCH - base_length[code], extra_lbits[code]);
        code = dist_code_of(z, dist);
        send(z, &dc[code]);
        if (extra_dbits[code])
            put_bits(z, dist - base_dist[code], extra_dbits[code]);
    }
    send(z, &lc[END_BLOCK]);
}

static void stored_block(AuDeflate *z, const uint8_t *p, uint32_t n, bool last)
{
    /* At most 65535 bytes each. */
    do {
        uint32_t take = n > 65535 ? 65535 : n;
        put_bits(z, (last && take == n) ? 1 : 0, 3); /* BFINAL, BTYPE 00 */
        align_bits(z);
        put_byte(z, take & 0xFF);
        put_byte(z, take >> 8);
        put_byte(z, ~take & 0xFF);
        put_byte(z, (~take >> 8) & 0xFF);
        au_buf_append(&z->pend, p, take);
        p += take;
        n -= take;
    } while (n);
}

/* Writes the collected block in its smallest form. */
static void flush_block(AuDeflate *z, bool last)
{
    z->lfreq[END_BLOCK] = 1;
    int max_l = build_lengths(z->lfreq, L_CODES, MAX_BITS, z->dyn_l);
    int max_d = build_lengths(z->dfreq, D_CODES, MAX_BITS, z->dyn_d);
    if (max_l < 256)
        max_l = 256;
    memset(z->blfreq, 0, sizeof z->blfreq);
    scan_lengths(z, z->dyn_l, max_l + 1, false);
    scan_lengths(z, z->dyn_d, max_d + 1, false);
    build_lengths(z->blfreq, BL_CODES, MAX_BL_BITS, z->bl);
    int nbl = BL_CODES;
    while (nbl > 4 && z->bl[bl_order[nbl - 1]].len == 0)
        nbl--;
    /* Sizes in bits. */
    uint32_t dyn = 3 + 5 + 5 + 4 + 3 * (uint32_t)nbl, fix = 3;
    for (int i = 0; i < BL_CODES; i++)
        dyn += z->blfreq[i] * (z->bl[i].len + (i == 16 ? 2u : i == 17 ? 3u : i == 18 ? 7u : 0u));
    for (int i = 0; i < L_CODES; i++) {
        uint32_t extra = i > 256 ? extra_lbits[i - 257] : 0;
        dyn += z->lfreq[i] * (z->dyn_l[i].len + extra);
        fix += z->lfreq[i] * (z->fixed_l[i].len + extra);
    }
    for (int i = 0; i < D_CODES; i++) {
        dyn += z->dfreq[i] * (z->dyn_d[i].len + extra_dbits[i]);
        fix += z->dfreq[i] * (5u + extra_dbits[i]);
    }
    uint32_t best = dyn < fix ? dyn : fix;
    uint32_t stored_bits = (z->block_bytes + 5 * (z->block_bytes / 65535 + 1)) * 8;
    if (z->block_start >= 0 && (z->level == 0 || stored_bits <= best)) {
        stored_block(z, z->win + z->block_start, z->block_bytes, last);
    } else if (fix <= dyn) {
        put_bits(z, (last ? 1u : 0u) | (1u << 1), 3);
        send_symbols(z, z->fixed_l, z->fixed_d);
    } else {
        put_bits(z, (last ? 1u : 0u) | (2u << 1), 3);
        put_bits(z, (unsigned)(max_l + 1 - 257), 5);
        put_bits(z, (unsigned)(max_d + 1 - 1), 5);
        put_bits(z, (unsigned)(nbl - 4), 4);
        for (int i = 0; i < nbl; i++)
            put_bits(z, z->bl[bl_order[i]].len, 3);
        scan_lengths(z, z->dyn_l, max_l + 1, true);
        scan_lengths(z, z->dyn_d, max_d + 1, true);
        send_symbols(z, z->dyn_l, z->dyn_d);
    }
    if (last)
        align_bits(z);
    z->block_start = (long)z->strstart;
    block_reset(z);
}

/* ---- Window and matching ---- */

static uint32_t hash3(const uint8_t *p)
{
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) & HASH_MASK;
}

/* Enters the string at pos in the chains; returns the position that had
 * its hash before. pos + 2 must be data. */
static uint32_t insert(AuDeflate *z, uint32_t pos)
{
    uint32_t h = hash3(z->win + pos);
    uint32_t before = z->head[h];
    z->prev[pos & WMASK] = (uint16_t)before;
    z->head[h] = (uint16_t)pos;
    return before;
}

/* Takes input into the window, sliding it down when the upper half is
 * used up. */
static void fill_window(AuDeflate *z, const uint8_t **in, size_t *in_len)
{
    while (*in_len && z->lookahead < MIN_LOOKAHEAD) {
        if (z->strstart >= WSIZE + MAX_DIST) {
            memmove(z->win, z->win + WSIZE, WSIZE);
            z->match_start = z->match_start >= WSIZE ? z->match_start - WSIZE : 0;
            z->prev_match = z->prev_match >= WSIZE ? z->prev_match - WSIZE : 0;
            z->strstart -= WSIZE;
            z->block_start -= (long)WSIZE;
            for (uint32_t i = 0; i < HASH_SIZE; i++)
                z->head[i] = (uint16_t)(z->head[i] >= WSIZE ? z->head[i] - WSIZE : NIL);
            for (uint32_t i = 0; i < WSIZE; i++)
                z->prev[i] = (uint16_t)(z->prev[i] >= WSIZE ? z->prev[i] - WSIZE : NIL);
        }
        size_t room = 2 * WSIZE - z->strstart - z->lookahead;
        size_t n = *in_len < room ? *in_len : room;
        const uint8_t *src = *in;
        memcpy(z->win + z->strstart + z->lookahead, src, n);
        if (z->fmt == AU_DEFLATE_GZIP)
            z->check = au_codec_crc32(z->check, src, n);
        else if (z->fmt == AU_DEFLATE_ZLIB)
            z->check = au_codec_adler32(z->check, src, n);
        z->total_in += (uint32_t)n;
        z->lookahead += (uint32_t)n;
        *in += n;
        *in_len -= n;
    }
}

/* The longest match for the string at strstart among the chain starting
 * at cur, longer than best; sets match_start. */
static uint32_t longest_match(AuDeflate *z, uint32_t cur, uint32_t best)
{
    const uint8_t *win = z->win, *scan = win + z->strstart;
    uint32_t chain = z->max_chain;
    uint32_t maxlen = z->lookahead < MAX_MATCH ? z->lookahead : MAX_MATCH;
    uint32_t limit = z->strstart > MAX_DIST ? z->strstart - MAX_DIST : NIL;
    uint32_t nice = z->nice_len < maxlen ? z->nice_len : maxlen;
    if (best >= maxlen)
        return best;
    if (best >= z->good_len)
        chain >>= 2;
    do {
        if (cur >= z->strstart)
            break; /* not a position before this one: the chain is stale */
        const uint8_t *m = win + cur;
        if (m[best] != scan[best] || m[0] != scan[0] || m[1] != scan[1])
            continue;
        uint32_t len = 2;
        while (len < maxlen && m[len] == scan[len])
            len++;
        if (len > best) {
            z->match_start = cur;
            best = len;
            if (len >= nice)
                break;
        }
    } while ((cur = z->prev[cur & WMASK]) > limit && --chain);
    return best;
}

typedef enum { MORE_INPUT, BLOCK_OUT, FINISHED } Step;

/* Level 0: the input as stored blocks. */
static Step compress_stored(AuDeflate *z, const uint8_t **in, size_t *in_len, bool finish)
{
    for (;;) {
        fill_window(z, in, in_len);
        if (z->lookahead == 0 && !*in_len) {
            if (!finish)
                return MORE_INPUT;
            flush_block(z, true);
            return FINISHED;
        }
        z->block_bytes += z->lookahead;
        z->strstart += z->lookahead;
        z->lookahead = 0;
        if (z->block_bytes >= WSIZE - MIN_LOOKAHEAD) {
            flush_block(z, false);
            return BLOCK_OUT;
        }
    }
}

static Step compress_greedy(AuDeflate *z, const uint8_t **in, size_t *in_len, bool finish)
{
    for (;;) {
        if (z->lookahead < MIN_LOOKAHEAD) {
            fill_window(z, in, in_len);
            if (z->lookahead < MIN_LOOKAHEAD && !finish)
                return MORE_INPUT;
            if (z->lookahead == 0)
                break;
        }
        uint32_t cand = NIL, mlen = MIN_MATCH - 1;
        if (z->lookahead >= MIN_MATCH)
            cand = insert(z, z->strstart);
        if (cand > (z->strstart > MAX_DIST ? z->strstart - MAX_DIST : NIL))
            mlen = longest_match(z, cand, MIN_MATCH - 1);
        bool full;
        if (mlen >= MIN_MATCH) {
            full = tally(z, z->strstart - z->match_start, mlen - MIN_MATCH);
            z->lookahead -= mlen;
            /* The strings inside the match join the chains while three
             * bytes of data follow each. */
            uint32_t end = z->strstart + mlen, data_end = end + z->lookahead;
            for (uint32_t s = z->strstart + 1; s < end && s + MIN_MATCH <= data_end && mlen <= z->max_lazy; s++)
                insert(z, s);
            z->strstart = end;
        } else {
            full = tally(z, 0, z->win[z->strstart]);
            z->lookahead--;
            z->strstart++;
        }
        if (full) {
            flush_block(z, false);
            return BLOCK_OUT;
        }
    }
    flush_block(z, true);
    return FINISHED;
}

static Step compress_lazy(AuDeflate *z, const uint8_t **in, size_t *in_len, bool finish)
{
    for (;;) {
        if (z->lookahead < MIN_LOOKAHEAD) {
            fill_window(z, in, in_len);
            if (z->lookahead < MIN_LOOKAHEAD && !finish)
                return MORE_INPUT;
            if (z->lookahead == 0)
                break;
        }
        uint32_t cand = NIL;
        if (z->lookahead >= MIN_MATCH)
            cand = insert(z, z->strstart);
        /* The match found at the last position, and the one here. */
        z->prev_length = z->match_available ? z->prev_length : MIN_MATCH - 1;
        uint32_t prev_len = z->prev_length;
        z->prev_match = z->match_start;
        uint32_t mlen = MIN_MATCH - 1;
        if (prev_len < z->max_lazy && cand > (z->strstart > MAX_DIST ? z->strstart - MAX_DIST : NIL)) {
            uint32_t saved = z->match_start;
            mlen = longest_match(z, cand, prev_len > MIN_MATCH - 1 ? prev_len : MIN_MATCH - 1);
            if (mlen <= prev_len || mlen < MIN_MATCH) {
                z->match_start = saved; /* nothing longer here */
                mlen = MIN_MATCH - 1;
            } else if (mlen == MIN_MATCH && z->strstart - z->match_start > TOO_FAR) {
                z->match_start = saved;
                mlen = MIN_MATCH - 1;
            }
        }
        bool full = false;
        if (z->match_available && prev_len >= MIN_MATCH && mlen <= prev_len) {
            /* The match at the last position stands. */
            uint32_t data_end = z->strstart + z->lookahead;
            full = tally(z, z->strstart - 1 - z->prev_match, prev_len - MIN_MATCH);
            uint32_t end = z->strstart - 1 + prev_len;
            for (uint32_t s = z->strstart + 1; s < end; s++)
                if (s + MIN_MATCH <= data_end)
                    insert(z, s);
            z->lookahead -= prev_len - 1;
            z->strstart = end;
            z->match_available = false;
            z->prev_length = MIN_MATCH - 1;
        } else if (z->match_available) {
            /* A longer match starts here: the last byte goes as a literal. */
            full = tally(z, 0, z->win[z->strstart - 1]);
            z->prev_length = mlen;
            z->strstart++;
            z->lookahead--;
        } else {
            z->match_available = true;
            z->prev_length = mlen;
            z->strstart++;
            z->lookahead--;
        }
        if (full) {
            /* The block ends before the byte still waiting, if any. */
            long keep = z->match_available ? 1 : 0;
            z->strstart -= (uint32_t)keep;
            flush_block(z, false);
            z->strstart += (uint32_t)keep;
            return BLOCK_OUT;
        }
    }
    if (z->match_available) {
        /* The last byte: a match found for it would have been written by
         * the pass for the byte after it, so only a literal can wait. */
        tally(z, 0, z->win[z->strstart - 1]);
        z->match_available = false;
    }
    flush_block(z, true);
    return FINISHED;
}

/* ---- Interface ---- */

static void set_level(AuDeflate *z, int level)
{
    z->level = level < 0 ? 0 : level > 2 ? 2 : level;
    if (z->level == 1) {
        z->max_chain = 8;
        z->nice_len = 32;
        z->good_len = 8;
        z->max_lazy = 16; /* greedy: the longest match whose inner strings are entered */
        z->lazy = false;
    } else {
        z->max_chain = 96;
        z->nice_len = 128;
        z->good_len = 16;
        z->max_lazy = 32;
        z->lazy = true;
    }
}

AuDeflate *au_deflate_new(AuDeflateFormat fmt, int level)
{
    AuDeflate *z = au_calloc(sizeof *z);
    z->fmt = fmt;
    set_level(z, level);
    z->win = au_calloc(2 * WSIZE + WIN_SLACK);
    z->head = au_calloc(HASH_SIZE * sizeof *z->head);
    z->prev = au_calloc(WSIZE * sizeof *z->prev);
    z->d_buf = au_alloc(SYM_MAX * sizeof *z->d_buf);
    z->l_buf = au_alloc(SYM_MAX);
    au_buf_init(&z->pend);
    init_tables(z);
    au_deflate_reset(z);
    return z;
}

void au_deflate_free(AuDeflate *z)
{
    if (!z)
        return;
    au_free(z->win);
    au_free(z->head);
    au_free(z->prev);
    au_free(z->d_buf);
    au_free(z->l_buf);
    au_buf_free(&z->pend);
    au_free(z);
}

void au_deflate_reset(AuDeflate *z)
{
    memset(z->head, 0, HASH_SIZE * sizeof *z->head);
    memset(z->prev, 0, WSIZE * sizeof *z->prev);
    /* Position 0 means "none" in the chains, so data starts at 1. */
    z->win[0] = 0;
    z->strstart = 1;
    z->block_start = 1;
    z->lookahead = 0;
    z->match_start = 0;
    z->prev_match = 0;
    z->prev_length = MIN_MATCH - 1;
    z->match_available = false;
    block_reset(z);
    au_buf_clear(&z->pend);
    z->pend_pos = 0;
    z->bi_buf = 0;
    z->bi_valid = 0;
    z->check = z->fmt == AU_DEFLATE_ZLIB ? 1 : 0;
    z->total_in = 0;
    z->stage = ST_HEADER;
}

static void write_header(AuDeflate *z)
{
    if (z->fmt == AU_DEFLATE_GZIP) {
        static const uint8_t h[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 255}; /* no name, no time, OS unknown */
        au_buf_append(&z->pend, h, sizeof h);
    } else if (z->fmt == AU_DEFLATE_ZLIB) {
        unsigned cmf = 0x78, flg = (unsigned)(z->level == 0 ? 0 : z->level == 1 ? 1 : 2) << 6;
        flg += 31 - ((cmf << 8) + flg) % 31;
        put_byte(z, cmf);
        put_byte(z, flg);
    }
}

static void write_trailer(AuDeflate *z)
{
    if (z->fmt == AU_DEFLATE_GZIP) {
        for (int i = 0; i < 4; i++)
            put_byte(z, (z->check >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; i++)
            put_byte(z, (z->total_in >> (8 * i)) & 0xFF);
    } else if (z->fmt == AU_DEFLATE_ZLIB) {
        for (int i = 3; i >= 0; i--)
            put_byte(z, (z->check >> (8 * i)) & 0xFF);
    }
}

AuCodecStatus au_deflate_run(AuDeflate *z, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len,
                             bool finish)
{
    for (;;) {
        /* What is waiting goes out first. */
        if (z->pend_pos < z->pend.len) {
            size_t n = z->pend.len - z->pend_pos;
            if (n > *out_len)
                n = *out_len;
            memcpy(*out, z->pend.data + z->pend_pos, n);
            *out += n;
            *out_len -= n;
            z->pend_pos += n;
            if (z->pend_pos < z->pend.len)
                return AU_CODEC_OK; /* the output is full */
        }
        au_buf_clear(&z->pend);
        z->pend_pos = 0;
        switch (z->stage) {
        case ST_HEADER:
            write_header(z);
            z->stage = ST_BODY;
            break;
        case ST_BODY: {
            Step s = z->level == 0 ? compress_stored(z, in, in_len, finish)
                     : z->lazy     ? compress_lazy(z, in, in_len, finish)
                                   : compress_greedy(z, in, in_len, finish);
            if (s == MORE_INPUT)
                return AU_CODEC_OK;
            if (s == FINISHED)
                z->stage = ST_TRAILER;
            break;
        }
        case ST_TRAILER:
            write_trailer(z);
            z->stage = ST_DONE;
            break;
        case ST_DONE:
            return AU_CODEC_DONE;
        }
    }
}

void au_deflate_all(AuDeflateFormat fmt, int level, const void *data, size_t len, AuBuf *out)
{
    AuDeflate *z = au_deflate_new(fmt, level);
    const uint8_t *in = data;
    size_t in_len = len;
    for (;;) {
        au_buf_reserve(out, 16384);
        uint8_t *op = (uint8_t *)out->data + out->len;
        size_t room = out->cap - out->len, left = room;
        AuCodecStatus st = au_deflate_run(z, &in, &in_len, &op, &left, true);
        out->len += room - left;
        if (st == AU_CODEC_DONE)
            break;
    }
    au_deflate_free(z);
}
