/* Inflate (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952) framing.
 *
 * A streaming state machine. Input is taken one byte at a time, only when
 * a step needs more bits, so after the end of a stream the bytes that
 * follow it are still in the caller's input. Decoded bytes go into a 32 KB
 * circular window and are copied from there to the caller's output; a
 * step runs only when the window has room for its largest result (a
 * 258-byte match), so a step is never interrupted halfway.
 *
 * Huffman codes are decoded through a 9-bit lookup table (one probe for
 * codes up to 9 bits, which covers most literals and lengths), falling
 * back to a canonical walk for longer codes. */
#include "codec.h"

#include <string.h>

/* ---- Checksums ---- */

static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t au_codec_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return ~crc;
}

uint32_t au_codec_adler32(uint32_t adler, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (len) {
        /* 5552 is the largest n with 255n(n+1)/2 + (n+1)(65520) < 2^32. */
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/* ---- Huffman tables ---- */

#define FAST_BITS 9
#define MAX_BITS 15

typedef struct Huff {
    uint16_t fast[1 << FAST_BITS]; /* (symbol << 4) | length; 0 when the code is longer */
    uint16_t count[MAX_BITS + 1];  /* codes per length */
    uint16_t symbol[288];          /* symbols in canonical order */
} Huff;

static uint32_t reverse_bits(uint32_t code, int len)
{
    uint32_t r = 0;
    while (len--) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Builds a table from n code lengths. Returns false for an over-subscribed
 * set, or an incomplete one other than a single code (which RFC 1951
 * allows for distances and zlib accepts generally). */
static bool huff_build(Huff *h, const uint8_t *lens, int n)
{
    uint16_t offs[MAX_BITS + 2];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++)
        h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1, max = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return false;
        if (h->count[len])
            max = len;
    }
    if (left > 0 && max > 1)
        return false;
    offs[1] = 0;
    for (int len = 1; len <= MAX_BITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i])
            h->symbol[offs[lens[i]]++] = (uint16_t)i;
    /* Fast table: walk codes in canonical order. */
    uint32_t code = 0;
    int idx = 0;
    for (int len = 1; len <= FAST_BITS; len++) {
        for (int k = 0; k < h->count[len]; k++, idx++) {
            uint32_t rev = reverse_bits(code + (uint32_t)k, len);
            uint16_t e = (uint16_t)((h->symbol[idx] << 4) | len);
            for (uint32_t f = rev; f < (1u << FAST_BITS); f += 1u << len)
                h->fast[f] = e;
        }
        code = (code + h->count[len]) << 1;
    }
    return true;
}

/* ---- State ---- */

enum {
    S_HTTP_SNIFF,
    S_ZLIB_HEADER,
    S_GZIP_HEADER,
    S_BLOCK,
    S_STORED_LEN,
    S_STORED,
    S_DYN_COUNTS,
    S_DYN_CL,
    S_DYN_LENS,
    S_SYMBOL,
    S_LEN_EXTRA,
    S_DIST,
    S_DIST_EXTRA,
    S_TRAILER,
    S_GZIP_NEXT,
    S_DONE,
    S_ERROR
};

#define WSIZE 32768u
#define WMASK (WSIZE - 1)

struct AuInflate {
    AuDecoder dec; /* first, so the decoder interface can cast */
    AuInflateFormat fmt;
    int state;
    bool last;       /* the current block is the final one */
    bool raw_stream; /* format resolved to raw */
    bool gzip;
    uint32_t bitbuf;
    int bitcnt;
    /* gzip header parsing */
    int gz_step, gz_flags;
    uint32_t gz_skip;
    /* stored block */
    uint32_t stored_left;
    /* dynamic tables */
    int hlit, hdist, hclen, nlens;
    uint8_t lens[320];
    /* the current match */
    int sym, extra;
    uint32_t length, dist;
    /* checksums of the output */
    uint32_t check;
    uint32_t isize;
    int trailer_got;
    uint8_t trailer[8];
    /* output window */
    uint8_t *window;
    uint32_t wpos;    /* next write position (mod WSIZE) */
    uint32_t pending; /* bytes in the window not yet copied out */
    uint32_t have;    /* bytes of history available (capped at WSIZE) */
    const char *err;
    bool fixed_ready;
    Huff lit, distt;
};

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static AuCodecStatus fail(AuInflate *z, const char *msg)
{
    z->state = S_ERROR;
    z->err = msg;
    return AU_CODEC_ERROR;
}

/* Input cursor for one call. */
typedef struct In {
    const uint8_t *p;
    size_t n;
} In;

/* Makes at least nbits (<= 24) available. Returns false when the input
 * ran out first; the bits gathered so far stay in the buffer. */
static inline bool need(AuInflate *z, In *in, int nbits)
{
    while (z->bitcnt < nbits) {
        if (!in->n)
            return false;
        z->bitbuf |= (uint32_t)*in->p++ << z->bitcnt;
        in->n--;
        z->bitcnt += 8;
    }
    return true;
}

static inline uint32_t take(AuInflate *z, int nbits)
{
    uint32_t v = z->bitbuf & ((1u << nbits) - 1);
    z->bitbuf >>= nbits;
    z->bitcnt -= nbits;
    return v;
}

/* Reads a whole byte at a byte boundary (after the bit buffer is aligned). */
static bool get_byte(AuInflate *z, In *in, uint8_t *b)
{
    if (z->bitcnt >= 8) {
        *b = (uint8_t)take(z, 8);
        return true;
    }
    if (!in->n)
        return false;
    *b = *in->p++;
    in->n--;
    return true;
}

static void align_byte(AuInflate *z)
{
    take(z, z->bitcnt & 7);
}

/* Decodes one symbol. Returns the symbol, -1 when more input is needed,
 * -2 for an invalid code. Consumes nothing unless a symbol is returned. */
static int decode(AuInflate *z, In *in, const Huff *h)
{
    /* Gather what we can up to the longest code, without demanding it. */
    while (z->bitcnt < MAX_BITS && in->n) {
        z->bitbuf |= (uint32_t)*in->p++ << z->bitcnt;
        in->n--;
        z->bitcnt += 8;
    }
    uint16_t e = h->fast[z->bitbuf & ((1u << FAST_BITS) - 1)];
    if (e) {
        int len = e & 15;
        if (len > z->bitcnt)
            return -1;
        take(z, len);
        return e >> 4;
    }
    /* Canonical walk, bit by bit (codes are packed MSB first). */
    int code = 0, first = 0, index = 0;
    uint32_t bits = z->bitbuf;
    for (int len = 1; len <= MAX_BITS; len++) {
        if (len > z->bitcnt)
            return -1;
        code |= (int)(bits & 1);
        bits >>= 1;
        int count = h->count[len];
        if (code - first < count) {
            take(z, len);
            return h->symbol[index + code - first];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static void put_byte(AuInflate *z, uint8_t b)
{
    z->window[z->wpos] = b;
    z->wpos = (z->wpos + 1) & WMASK;
    z->pending++;
    if (z->have < WSIZE)
        z->have++;
}

static void build_fixed(AuInflate *z)
{
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++)
        l[i] = 8;
    for (; i < 256; i++)
        l[i] = 9;
    for (; i < 280; i++)
        l[i] = 7;
    for (; i < 288; i++)
        l[i] = 8;
    huff_build(&z->lit, l, 288);
    /* All 32 distance codes have 5 bits; 30 and 31 never occur in valid
     * data and are rejected when decoded. */
    for (i = 0; i < 32; i++)
        l[i] = 5;
    huff_build(&z->distt, l, 32);
}

/* Copies pending window bytes to the output and updates the checksum. */
static void flush(AuInflate *z, uint8_t **out, size_t *out_len)
{
    while (z->pending && *out_len) {
        uint32_t start = (z->wpos - z->pending) & WMASK;
        uint32_t run = WSIZE - start;
        if (run > z->pending)
            run = z->pending;
        if (run > *out_len)
            run = (uint32_t)*out_len;
        memcpy(*out, z->window + start, run);
        if (!z->raw_stream) {
            if (z->gzip)
                z->check = au_codec_crc32(z->check, *out, run);
            else
                z->check = au_codec_adler32(z->check, *out, run);
        }
        z->isize += run;
        *out += run;
        *out_len -= run;
        z->pending -= run;
    }
}

static void begin_stream(AuInflate *z)
{
    switch (z->fmt) {
    case AU_INFLATE_RAW:
        z->raw_stream = true;
        z->state = S_BLOCK;
        break;
    case AU_INFLATE_ZLIB:
        z->state = S_ZLIB_HEADER;
        break;
    case AU_INFLATE_GZIP:
        z->gzip = true;
        z->state = S_GZIP_HEADER;
        break;
    default:
        z->state = S_HTTP_SNIFF;
        break;
    }
}

void au_inflate_reset(AuInflate *z)
{
    z->last = false;
    z->raw_stream = z->gzip = false;
    z->bitbuf = 0;
    z->bitcnt = 0;
    z->gz_step = z->gz_flags = 0;
    z->gz_skip = 0;
    z->check = 1;
    z->isize = 0;
    z->trailer_got = 0;
    z->wpos = z->pending = z->have = 0;
    z->err = "";
    begin_stream(z);
    if (z->gzip)
        z->check = 0;
}

static AuCodecStatus dec_run(AuDecoder *d, const uint8_t **in, size_t *in_len, uint8_t **out, size_t *out_len)
{
    return au_inflate_run((AuInflate *)d, in, in_len, out, out_len);
}
static void dec_free(AuDecoder *d) { au_inflate_free((AuInflate *)d); }
static const char *dec_error(const AuDecoder *d) { return au_inflate_error((const AuInflate *)d); }

AuInflate *au_inflate_new(AuInflateFormat fmt)
{
    AuInflate *z = au_calloc(sizeof *z);
    z->window = au_alloc(WSIZE);
    z->fmt = fmt;
    z->dec.run = dec_run;
    z->dec.free = dec_free;
    z->dec.error = dec_error;
    au_inflate_reset(z);
    return z;
}

void au_inflate_free(AuInflate *z)
{
    if (!z)
        return;
    au_free(z->window);
    au_free(z);
}

const char *au_inflate_error(const AuInflate *z)
{
    return z->err ? z->err : "";
}

AuDecoder *au_inflate_decoder(AuInflateFormat fmt)
{
    return &au_inflate_new(fmt)->dec;
}

/* Parses the gzip header byte by byte. Returns 1 when done, 0 for more
 * input, -1 on error. */
static int gzip_header(AuInflate *z, In *in)
{
    uint8_t b;
    for (;;) {
        switch (z->gz_step) {
        case 0: /* ID1 ID2 CM FLG, MTIME(4) XFL OS: 10 bytes */
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            if (!get_byte(z, in, &b))
                return 0;
            if ((z->gz_step == 0 && b != 0x1F) || (z->gz_step == 1 && b != 0x8B) || (z->gz_step == 2 && b != 8)) {
                z->err = "not a gzip stream";
                return -1;
            }
            if (z->gz_step == 3) {
                if (b & 0xE0) {
                    z->err = "reserved gzip flags set";
                    return -1;
                }
                z->gz_flags = b;
            }
            z->gz_step++;
            if (z->gz_step == 10)
                z->gz_step = (z->gz_flags & 4) ? 10 : 13;
            break;
        case 10: /* XLEN low */
        case 11: /* XLEN high */
            if (!get_byte(z, in, &b))
                return 0;
            if (z->gz_step == 10)
                z->gz_skip = b;
            else
                z->gz_skip |= (uint32_t)b << 8;
            z->gz_step++;
            break;
        case 12: /* extra field */
            while (z->gz_skip) {
                if (!get_byte(z, in, &b))
                    return 0;
                z->gz_skip--;
            }
            z->gz_step = 13;
            break;
        case 13: /* file name */
            if (z->gz_flags & 8) {
                do {
                    if (!get_byte(z, in, &b))
                        return 0;
                } while (b);
            }
            z->gz_step = 14;
            break;
        case 14: /* comment */
            if (z->gz_flags & 16) {
                do {
                    if (!get_byte(z, in, &b))
                        return 0;
                } while (b);
            }
            z->gz_step = 15;
            z->gz_skip = (z->gz_flags & 2) ? 2 : 0;
            break;
        case 15: /* header CRC (not checked) */
            while (z->gz_skip) {
                if (!get_byte(z, in, &b))
                    return 0;
                z->gz_skip--;
            }
            return 1;
        default:
            return -1;
        }
    }
}

AuCodecStatus au_inflate_run(AuInflate *z, const uint8_t **inp, size_t *in_len, uint8_t **out, size_t *out_len)
{
    In in = {*inp, *in_len};
    AuCodecStatus st = AU_CODEC_OK;

#define RETURN(s)              \
    do {                       \
        st = (s);              \
        goto leave;            \
    } while (0)
#define FAIL(msg)              \
    do {                       \
        st = fail(z, msg);     \
        goto leave;            \
    } while (0)

    flush(z, out, out_len);
    for (;;) {
        switch (z->state) {
        case S_HTTP_SNIFF: {
            /* zlib header: CM 8, CINFO <= 7, FCHECK. Otherwise raw. */
            if (!need(z, &in, 16))
                RETURN(AU_CODEC_OK);
            uint32_t b0 = z->bitbuf & 0xFF, b1 = (z->bitbuf >> 8) & 0xFF;
            if ((b0 & 0x0F) == 8 && (b0 >> 4) <= 7 && ((b0 << 8) | b1) % 31 == 0) {
                z->state = S_ZLIB_HEADER;
            } else {
                z->raw_stream = true;
                z->state = S_BLOCK;
            }
            break;
        }
        case S_ZLIB_HEADER: {
            if (!need(z, &in, 16))
                RETURN(AU_CODEC_OK);
            uint32_t b0 = take(z, 8), b1 = take(z, 8);
            if ((b0 & 0x0F) != 8 || (b0 >> 4) > 7 || ((b0 << 8) | b1) % 31 != 0)
                FAIL("bad zlib header");
            if (b1 & 0x20)
                FAIL("zlib preset dictionary not supported");
            z->state = S_BLOCK;
            break;
        }
        case S_GZIP_HEADER: {
            int r = gzip_header(z, &in);
            if (r < 0)
                FAIL(z->err);
            if (r == 0)
                RETURN(AU_CODEC_OK);
            z->state = S_BLOCK;
            break;
        }
        case S_BLOCK: {
            if (z->last) {
                z->state = S_TRAILER;
                break;
            }
            if (!need(z, &in, 3))
                RETURN(AU_CODEC_OK);
            z->last = take(z, 1) != 0;
            uint32_t type = take(z, 2);
            if (type == 0) {
                align_byte(z);
                z->state = S_STORED_LEN;
            } else if (type == 1) {
                build_fixed(z);
                z->state = S_SYMBOL;
            } else if (type == 2) {
                z->state = S_DYN_COUNTS;
            } else {
                FAIL("invalid block type");
            }
            break;
        }
        case S_STORED_LEN: {
            /* Aligned, so bitcnt is a multiple of 8 and at most 24: need()
             * fills exactly to 32 bits. */
            if (!need(z, &in, 32))
                RETURN(AU_CODEC_OK);
            uint32_t len = z->bitbuf & 0xFFFF, nlen = z->bitbuf >> 16;
            z->bitbuf = 0;
            z->bitcnt = 0;
            if ((len ^ 0xFFFF) != nlen)
                FAIL("stored block length check failed");
            z->stored_left = len;
            z->state = S_STORED;
            break;
        }
        case S_STORED: {
            while (z->stored_left) {
                if (z->pending == WSIZE) {
                    flush(z, out, out_len);
                    if (z->pending == WSIZE)
                        RETURN(AU_CODEC_OK);
                }
                if (!in.n)
                    RETURN(AU_CODEC_OK);
                uint32_t room = WSIZE - z->pending;
                uint32_t contig = WSIZE - z->wpos;
                uint32_t n = z->stored_left;
                if (n > room)
                    n = room;
                if (n > contig)
                    n = contig;
                if (n > in.n)
                    n = (uint32_t)in.n;
                memcpy(z->window + z->wpos, in.p, n);
                in.p += n;
                in.n -= n;
                z->wpos = (z->wpos + n) & WMASK;
                z->pending += n;
                z->have = z->have + n > WSIZE ? WSIZE : z->have + n;
                z->stored_left -= n;
            }
            z->state = S_BLOCK;
            break;
        }
        case S_DYN_COUNTS:
            if (!need(z, &in, 14))
                RETURN(AU_CODEC_OK);
            z->hlit = (int)take(z, 5) + 257;
            z->hdist = (int)take(z, 5) + 1;
            z->hclen = (int)take(z, 4) + 4;
            if (z->hlit > 286 || z->hdist > 30)
                FAIL("too many length or distance codes");
            memset(z->lens, 0, sizeof z->lens);
            z->nlens = 0;
            z->state = S_DYN_CL;
            break;
        case S_DYN_CL:
            while (z->nlens < z->hclen) {
                if (!need(z, &in, 3))
                    RETURN(AU_CODEC_OK);
                z->lens[cl_order[z->nlens++]] = (uint8_t)take(z, 3);
            }
            /* The code length code goes in distt temporarily. */
            if (!huff_build(&z->distt, z->lens, 19))
                FAIL("invalid code length code");
            {
                int any = 0;
                for (int i = 0; i < 19; i++)
                    any |= z->lens[i];
                if (!any)
                    FAIL("empty code length code");
            }
            memset(z->lens, 0, sizeof z->lens);
            z->nlens = 0;
            z->sym = -1;
            z->state = S_DYN_LENS;
            break;
        case S_DYN_LENS: {
            int total = z->hlit + z->hdist;
            while (z->nlens < total) {
                /* A repeat symbol whose extra bits had not arrived is kept
                 * in z->sym (-1 when none). */
                int sym = z->sym;
                if (sym < 0) {
                    sym = decode(z, &in, &z->distt);
                    if (sym == -2)
                        FAIL("invalid code length symbol");
                    if (sym < 0)
                        RETURN(AU_CODEC_OK);
                    if (sym < 16) {
                        z->lens[z->nlens++] = (uint8_t)sym;
                        continue;
                    }
                }
                int nb = sym == 16 ? 2 : sym == 17 ? 3 : 7;
                if (!need(z, &in, nb)) {
                    z->sym = sym;
                    RETURN(AU_CODEC_OK);
                }
                z->sym = -1;
                int rep, val = 0;
                if (sym == 16) {
                    if (z->nlens == 0)
                        FAIL("repeat with no previous length");
                    val = z->lens[z->nlens - 1];
                    rep = 3 + (int)take(z, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)take(z, 3);
                } else {
                    rep = 11 + (int)take(z, 7);
                }
                if (z->nlens + rep > total)
                    FAIL("code lengths overflow");
                while (rep--)
                    z->lens[z->nlens++] = (uint8_t)val;
            }
            if (z->lens[256] == 0)
                FAIL("no end-of-block code");
            if (!huff_build(&z->lit, z->lens, z->hlit))
                FAIL("invalid literal/length code");
            if (!huff_build(&z->distt, z->lens + z->hlit, z->hdist))
                FAIL("invalid distance code");
            z->state = S_SYMBOL;
            break;
        }
        case S_SYMBOL:
            for (;;) {
                if (z->pending + 258 > WSIZE) {
                    flush(z, out, out_len);
                    if (z->pending + 258 > WSIZE)
                        RETURN(AU_CODEC_OK);
                }
                int sym = decode(z, &in, &z->lit);
                if (sym < 0) {
                    if (sym == -2)
                        FAIL("invalid literal/length code");
                    RETURN(AU_CODEC_OK);
                }
                if (sym < 256) {
                    put_byte(z, (uint8_t)sym);
                    continue;
                }
                if (sym == 256) {
                    z->state = S_BLOCK;
                    break;
                }
                sym -= 257;
                if (sym >= 29)
                    FAIL("invalid length symbol");
                z->sym = sym;
                z->state = S_LEN_EXTRA;
                break;
            }
            break;
        case S_LEN_EXTRA:
            if (!need(z, &in, len_extra[z->sym]))
                RETURN(AU_CODEC_OK);
            z->length = len_base[z->sym] + take(z, len_extra[z->sym]);
            z->state = S_DIST;
            break;
        case S_DIST: {
            int sym = decode(z, &in, &z->distt);
            if (sym == -2)
                FAIL("invalid distance code");
            if (sym < 0)
                RETURN(AU_CODEC_OK);
            if (sym >= 30)
                FAIL("invalid distance symbol");
            z->sym = sym;
            z->state = S_DIST_EXTRA;
            break;
        }
        case S_DIST_EXTRA: {
            if (!need(z, &in, dist_extra[z->sym]))
                RETURN(AU_CODEC_OK);
            uint32_t dist = dist_base[z->sym] + take(z, dist_extra[z->sym]);
            if (dist > z->have)
                FAIL("distance too far back");
            /* Room for the match was checked before the length symbol. */
            uint32_t from = (z->wpos - dist) & WMASK;
            for (uint32_t k = 0; k < z->length; k++) {
                z->window[z->wpos] = z->window[from];
                z->wpos = (z->wpos + 1) & WMASK;
                from = (from + 1) & WMASK;
            }
            z->pending += z->length;
            z->have = z->have + z->length > WSIZE ? WSIZE : z->have + z->length;
            z->state = S_SYMBOL;
            break;
        }
        case S_TRAILER: {
            int want = z->raw_stream ? 0 : z->gzip ? 8 : 4;
            /* The checksum covers all output: flush everything first. */
            flush(z, out, out_len);
            if (z->pending)
                RETURN(AU_CODEC_OK);
            if (z->trailer_got == 0)
                align_byte(z);
            while (z->trailer_got < want) {
                uint8_t b;
                if (!get_byte(z, &in, &b))
                    RETURN(AU_CODEC_OK);
                z->trailer[z->trailer_got++] = b;
            }
            if (z->gzip) {
                uint32_t crc = (uint32_t)z->trailer[0] | ((uint32_t)z->trailer[1] << 8) |
                               ((uint32_t)z->trailer[2] << 16) | ((uint32_t)z->trailer[3] << 24);
                uint32_t size = (uint32_t)z->trailer[4] | ((uint32_t)z->trailer[5] << 8) |
                                ((uint32_t)z->trailer[6] << 16) | ((uint32_t)z->trailer[7] << 24);
                if (crc != z->check)
                    FAIL("gzip CRC mismatch");
                if (size != z->isize)
                    FAIL("gzip size mismatch");
                z->state = S_GZIP_NEXT;
            } else if (!z->raw_stream) {
                uint32_t a = ((uint32_t)z->trailer[0] << 24) | ((uint32_t)z->trailer[1] << 16) |
                             ((uint32_t)z->trailer[2] << 8) | (uint32_t)z->trailer[3];
                if (a != z->check)
                    FAIL("zlib Adler-32 mismatch");
                z->state = S_DONE;
            } else {
                z->state = S_DONE;
            }
            /* Give back whole bytes still in the bit buffer (raw streams
             * end mid-byte; those bits are padding). */
            align_byte(z);
            while (z->bitcnt >= 8 && in.p > *inp) {
                /* The newest bytes in bitbuf came from this call's input,
                 * in order: step the cursor back over them. Bytes from an
                 * earlier call stay in bitbuf (at most 3). */
                in.p--;
                in.n++;
                z->bitcnt -= 8;
                z->bitbuf &= z->bitcnt ? (1u << z->bitcnt) - 1 : 0;
            }
            break;
        }
        case S_GZIP_NEXT: {
            /* Another member may follow. */
            int next;
            if (z->bitcnt >= 8)
                next = (int)(z->bitbuf & 0xFF);
            else if (in.n)
                next = in.p[0];
            else
                RETURN(AU_CODEC_DONE);
            if (next != 0x1F) {
                z->state = S_DONE;
                RETURN(AU_CODEC_DONE);
            }
            z->last = false;
            z->gz_step = z->gz_flags = 0;
            z->gz_skip = 0;
            z->check = 0;
            z->isize = 0;
            z->trailer_got = 0;
            z->state = S_GZIP_HEADER;
            break;
        }
        case S_DONE:
            RETURN(AU_CODEC_DONE);
        default:
            RETURN(AU_CODEC_ERROR);
        }
    }
leave:
    if (st != AU_CODEC_ERROR)
        flush(z, out, out_len);
    if (st == AU_CODEC_DONE && z->pending)
        st = AU_CODEC_OK; /* the caller must collect the rest first */
    *inp = in.p;
    *in_len = in.n;
    return st;
#undef RETURN
#undef FAIL
}

bool au_inflate_all(AuInflateFormat fmt, const void *in, size_t in_len, size_t max_out, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    AuInflate *z = au_inflate_new(fmt);
    size_t cap = in_len < 4096 ? 4096 : in_len;
    if (cap > max_out)
        cap = max_out;
    uint8_t *buf = au_alloc(cap ? cap : 1);
    size_t len = 0;
    const uint8_t *ip = in;
    size_t il = in_len;
    bool ok = false;
    for (;;) {
        uint8_t *op = buf + len;
        size_t ol = cap - len;
        AuCodecStatus st = au_inflate_run(z, &ip, &il, &op, &ol);
        len = (size_t)(op - buf);
        if (st == AU_CODEC_DONE) {
            ok = true;
            break;
        }
        if (st == AU_CODEC_ERROR)
            break;
        if (ol == 0) {
            if (cap >= max_out) {
                /* Full: the stream may still be done if nothing remains. */
                uint8_t probe;
                uint8_t *pp = &probe;
                size_t pl = 1;
                st = au_inflate_run(z, &ip, &il, &pp, &pl);
                ok = st == AU_CODEC_DONE && pl == 1;
                break;
            }
            size_t ncap = cap > max_out / 2 ? max_out : cap * 2;
            buf = au_realloc(buf, ncap);
            cap = ncap;
            continue;
        }
        if (il == 0)
            break; /* truncated */
    }
    au_inflate_free(z);
    if (!ok) {
        au_free(buf);
        return false;
    }
    *out = buf;
    *out_len = len;
    return true;
}
