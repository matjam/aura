/* Web font containers: WOFF 1.0 (zlib-compressed tables) and WOFF2
 * (brotli, with the transformed glyf/loca and hmtx tables, collections),
 * decoded into the sfnt bytes the engine loads. CFF-flavoured fonts are
 * refused as unsupported. Checksums are not recomputed.
 *
 * The input comes from the network: every read is bounds-checked, every
 * declared size is checked against the data and against the caller's
 * output limit (with 64-bit sums), counts are capped, and decompressed
 * sizes must match the declared ones exactly. */
#include "font_internal.h"

#include "../codec/brotli.h"

#include <string.h>

#define MAX_TABLES 256
#define MAX_FONTS 64      /* in a WOFF2 collection */
#define MAX_GLYPH_POINTS 20000

typedef struct In {
    const uint8_t *p;
    size_t len, at;
    bool bad;
} In;

static uint8_t in_u8(In *r)
{
    if (r->at >= r->len) {
        r->bad = true;
        return 0;
    }
    return r->p[r->at++];
}

static uint16_t in_u16(In *r)
{
    uint16_t hi = in_u8(r);
    return (uint16_t)((hi << 8) | in_u8(r));
}

static uint32_t in_u32(In *r)
{
    uint32_t v = in_u16(r);
    return (v << 16) | in_u16(r);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* UIntBase128: at most five bytes, no leading zeros, fits 32 bits. */
static uint32_t in_base128(In *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t b = in_u8(r);
        if (r->bad || (i == 0 && b == 0x80) || (v & 0xFE000000u)) {
            r->bad = true;
            return 0;
        }
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return v;
    }
    r->bad = true;
    return 0;
}

static uint16_t in_255u16(In *r)
{
    uint8_t c = in_u8(r);
    if (c == 253)
        return in_u16(r);
    if (c == 255)
        return (uint16_t)(253 + in_u8(r));
    if (c == 254)
        return (uint16_t)(506 + in_u8(r));
    return c;
}

AuFontFormat au_font_sniff(const void *data, size_t len)
{
    if (!data || len < 4)
        return AU_FONT_FORMAT_UNKNOWN;
    uint32_t v = be32(data);
    if (v == AU_TAG('w', 'O', 'F', 'F'))
        return AU_FONT_FORMAT_WOFF;
    if (v == AU_TAG('w', 'O', 'F', '2'))
        return AU_FONT_FORMAT_WOFF2;
    if (v == AU_TAG('O', 'T', 'T', 'O'))
        return AU_FONT_FORMAT_CFF;
    if (v == 0x00010000 || v == AU_TAG('t', 'r', 'u', 'e') || v == AU_TAG('t', 't', 'c', 'f'))
        return AU_FONT_FORMAT_SFNT;
    return AU_FONT_FORMAT_UNKNOWN;
}

/* ---- sfnt assembly ---- */

typedef struct Table {
    uint32_t tag;
    uint8_t *data; /* owned */
    uint32_t len;
    uint32_t out_off;
} Table;

static void tables_free(Table *t, size_t n)
{
    for (size_t i = 0; i < n; i++)
        au_free(t[i].data);
    au_free(t);
}

static uint32_t pad4(uint32_t n)
{
    return (n + 3) & ~3u;
}

static void write_dir(uint8_t *o, uint32_t flavor, const uint16_t *idx, size_t n, const Table *t)
{
    put32(o, flavor);
    put16(o + 4, (uint32_t)n);
    uint32_t es = 0;
    while ((2u << es) <= n)
        es++;
    uint32_t sr = (1u << es) * 16;
    put16(o + 6, sr);
    put16(o + 8, es);
    put16(o + 10, (uint32_t)n * 16 - sr);
    /* Records sorted by tag (indices are sorted by the caller). */
    for (size_t i = 0; i < n; i++) {
        const Table *e = &t[idx[i]];
        uint8_t *r = o + 12 + i * 16;
        put32(r, e->tag);
        put32(r + 4, 0); /* checksum: not recomputed */
        put32(r + 8, e->out_off);
        put32(r + 12, e->len);
    }
}

static void sort_idx(uint16_t *idx, size_t n, const Table *t)
{
    for (size_t i = 1; i < n; i++)
        for (size_t k = i; k > 0 && t[idx[k - 1]].tag > t[idx[k]].tag; k--) {
            uint16_t x = idx[k];
            idx[k] = idx[k - 1];
            idx[k - 1] = x;
        }
}

/* Lays out one or more fonts (fonts[f] lists indices into t) as an sfnt
 * or a TrueType collection. */
static AuFontStatus assemble(Table *t, size_t nt, uint16_t **fonts, const uint16_t *nfont_tables,
                             const uint32_t *flavors, size_t nfonts, size_t max_out, uint8_t **out, size_t *out_len)
{
    uint64_t size = nfonts > 1 ? 12 + 4 * (uint64_t)nfonts : 0;
    for (size_t f = 0; f < nfonts; f++)
        size += 12 + 16 * (uint64_t)nfont_tables[f];
    size = (size + 3) & ~(uint64_t)3;
    for (size_t i = 0; i < nt; i++) {
        t[i].out_off = (uint32_t)size;
        size += pad4(t[i].len);
        if (size > max_out)
            return AU_FONT_BAD;
    }
    uint8_t *o = au_calloc((size_t)size);
    uint32_t dir = 0;
    if (nfonts > 1) {
        put32(o, AU_TAG('t', 't', 'c', 'f'));
        put32(o + 4, 0x00010000);
        put32(o + 8, (uint32_t)nfonts);
        dir = 12 + 4 * (uint32_t)nfonts;
    }
    for (size_t f = 0; f < nfonts; f++) {
        if (nfonts > 1)
            put32(o + 12 + f * 4, dir);
        sort_idx(fonts[f], nfont_tables[f], t);
        write_dir(o + dir, flavors[f], fonts[f], nfont_tables[f], t);
        dir += 12 + 16 * (uint32_t)nfont_tables[f];
    }
    for (size_t i = 0; i < nt; i++)
        if (t[i].len)
            memcpy(o + t[i].out_off, t[i].data, t[i].len);
    *out = o;
    *out_len = (size_t)size;
    return AU_FONT_OK;
}

static bool is_cff(uint32_t tag)
{
    return tag == AU_TAG('C', 'F', 'F', ' ') || tag == AU_TAG('C', 'F', 'F', '2');
}

/* ---- WOFF 1.0 ---- */

static AuFontStatus decode_woff(const uint8_t *d, size_t len, size_t max_out, uint8_t **out, size_t *out_len)
{
    In r = {d, len, 0, false};
    if (len < 44 || in_u32(&r) != AU_TAG('w', 'O', 'F', 'F'))
        return AU_FONT_BAD;
    uint32_t flavor = in_u32(&r), length = in_u32(&r);
    uint16_t n = in_u16(&r);
    in_u16(&r); /* reserved */
    if (flavor == AU_TAG('O', 'T', 'T', 'O'))
        return AU_FONT_UNSUPPORTED;
    if (length != len || !n || n > MAX_TABLES || 44 + (size_t)n * 20 > len)
        return AU_FONT_BAD;
    Table *t = au_calloc(n * sizeof *t);
    uint16_t *idx = au_alloc(n * sizeof *idx);
    AuFontStatus st = AU_FONT_BAD;
    uint64_t total = 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *e = d + 44 + (size_t)i * 20;
        uint32_t tag = be32(e), off = be32(e + 4), clen = be32(e + 8), olen = be32(e + 12);
        idx[i] = i;
        t[i].tag = tag;
        if (is_cff(tag)) {
            st = AU_FONT_UNSUPPORTED;
            goto out;
        }
        if (off > len || clen > len - off || clen > olen || olen > max_out)
            goto out;
        total += pad4(olen);
        if (total > max_out)
            goto out;
        for (uint16_t k = 0; k < i; k++)
            if (t[k].tag == tag)
                goto out; /* a table twice */
        if (clen == olen) {
            t[i].data = au_alloc(olen ? olen : 1);
            memcpy(t[i].data, d + off, olen);
        } else {
            size_t got = 0;
            if (!au_inflate_all(AU_INFLATE_ZLIB, d + off, clen, olen, &t[i].data, &got) || got != olen) {
                au_free(t[i].data);
                t[i].data = NULL;
                goto out;
            }
        }
        t[i].len = olen;
    }
    uint32_t fl = flavor;
    st = assemble(t, n, &idx, &n, &fl, 1, max_out, out, out_len);
out:
    au_free(idx);
    tables_free(t, n);
    return st;
}

/* ---- WOFF2 ---- */

static const uint32_t known_tags[63] = {
    AU_TAG('c', 'm', 'a', 'p'), AU_TAG('h', 'e', 'a', 'd'), AU_TAG('h', 'h', 'e', 'a'), AU_TAG('h', 'm', 't', 'x'),
    AU_TAG('m', 'a', 'x', 'p'), AU_TAG('n', 'a', 'm', 'e'), AU_TAG('O', 'S', '/', '2'), AU_TAG('p', 'o', 's', 't'),
    AU_TAG('c', 'v', 't', ' '), AU_TAG('f', 'p', 'g', 'm'), AU_TAG('g', 'l', 'y', 'f'), AU_TAG('l', 'o', 'c', 'a'),
    AU_TAG('p', 'r', 'e', 'p'), AU_TAG('C', 'F', 'F', ' '), AU_TAG('V', 'O', 'R', 'G'), AU_TAG('E', 'B', 'D', 'T'),
    AU_TAG('E', 'B', 'L', 'C'), AU_TAG('g', 'a', 's', 'p'), AU_TAG('h', 'd', 'm', 'x'), AU_TAG('k', 'e', 'r', 'n'),
    AU_TAG('L', 'T', 'S', 'H'), AU_TAG('P', 'C', 'L', 'T'), AU_TAG('V', 'D', 'M', 'X'), AU_TAG('v', 'h', 'e', 'a'),
    AU_TAG('v', 'm', 't', 'x'), AU_TAG('B', 'A', 'S', 'E'), AU_TAG('G', 'D', 'E', 'F'), AU_TAG('G', 'P', 'O', 'S'),
    AU_TAG('G', 'S', 'U', 'B'), AU_TAG('E', 'B', 'S', 'C'), AU_TAG('J', 'S', 'T', 'F'), AU_TAG('M', 'A', 'T', 'H'),
    AU_TAG('C', 'B', 'D', 'T'), AU_TAG('C', 'B', 'L', 'C'), AU_TAG('C', 'O', 'L', 'R'), AU_TAG('C', 'P', 'A', 'L'),
    AU_TAG('S', 'V', 'G', ' '), AU_TAG('s', 'b', 'i', 'x'), AU_TAG('a', 'c', 'n', 't'), AU_TAG('a', 'v', 'a', 'r'),
    AU_TAG('b', 'd', 'a', 't'), AU_TAG('b', 'l', 'o', 'c'), AU_TAG('b', 's', 'l', 'n'), AU_TAG('c', 'v', 'a', 'r'),
    AU_TAG('f', 'd', 's', 'c'), AU_TAG('f', 'e', 'a', 't'), AU_TAG('f', 'm', 't', 'x'), AU_TAG('f', 'v', 'a', 'r'),
    AU_TAG('g', 'v', 'a', 'r'), AU_TAG('h', 's', 't', 'y'), AU_TAG('j', 'u', 's', 't'), AU_TAG('l', 'c', 'a', 'r'),
    AU_TAG('m', 'o', 'r', 't'), AU_TAG('m', 'o', 'r', 'x'), AU_TAG('o', 'p', 'b', 'd'), AU_TAG('p', 'r', 'o', 'p'),
    AU_TAG('t', 'r', 'a', 'k'), AU_TAG('Z', 'a', 'p', 'f'), AU_TAG('S', 'i', 'l', 'f'), AU_TAG('G', 'l', 'a', 't'),
    AU_TAG('G', 'l', 'o', 'c'), AU_TAG('F', 'e', 'a', 't'), AU_TAG('S', 'i', 'l', 'l'),
};

#define TAG_GLYF AU_TAG('g', 'l', 'y', 'f')
#define TAG_LOCA AU_TAG('l', 'o', 'c', 'a')
#define TAG_HMTX AU_TAG('h', 'm', 't', 'x')
#define TAG_HHEA AU_TAG('h', 'h', 'e', 'a')

typedef struct W2Entry {
    uint32_t tag, orig_len, stream_len, stream_off;
    uint8_t xform; /* transformation version */
    bool transformed;
} W2Entry;

/* A growing output buffer with a hard cap. */
typedef struct Out {
    uint8_t *p;
    size_t len, cap, limit;
    bool bad;
} Out;

static uint8_t *out_room(Out *o, size_t n)
{
    if (o->bad || n > o->limit - o->len) {
        o->bad = true;
        return NULL;
    }
    if (o->len + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 4096;
        while (c < o->len + n)
            c *= 2;
        if (c > o->limit)
            c = o->limit;
        o->p = au_realloc(o->p, c);
        o->cap = c;
    }
    uint8_t *q = o->p + o->len;
    o->len += n;
    return q;
}

static void out_u8(Out *o, uint32_t v)
{
    uint8_t *q = out_room(o, 1);
    if (q)
        q[0] = (uint8_t)v;
}

static void out_u16(Out *o, uint32_t v)
{
    uint8_t *q = out_room(o, 2);
    if (q)
        put16(q, v);
}

static int with_sign(int flag, int base)
{
    return (flag & 1) ? base : -base;
}

/* One point's coordinate deltas from the glyph stream (triplet
 * encoding). */
static bool triplet(In *g, uint8_t flag, int *dx, int *dy)
{
    flag &= 0x7F;
    if (flag < 10) {
        *dx = 0;
        *dy = with_sign(flag, ((flag & 14) << 7) + in_u8(g));
    } else if (flag < 20) {
        *dx = with_sign(flag, (((flag - 10) & 14) << 7) + in_u8(g));
        *dy = 0;
    } else if (flag < 84) {
        int b0 = flag - 20, b1 = in_u8(g);
        *dx = with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
        *dy = with_sign(flag >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
    } else if (flag < 120) {
        int b0 = flag - 84, b1 = in_u8(g), b2 = in_u8(g);
        *dx = with_sign(flag, 1 + ((b0 / 12) << 8) + b1);
        *dy = with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + b2);
    } else if (flag < 124) {
        int b1 = in_u8(g), b2 = in_u8(g), b3 = in_u8(g);
        *dx = with_sign(flag, (b1 << 4) + (b2 >> 4));
        *dy = with_sign(flag >> 1, ((b2 & 0x0F) << 8) + b3);
    } else {
        int b1 = in_u8(g), b2 = in_u8(g), b3 = in_u8(g), b4 = in_u8(g);
        *dx = with_sign(flag, (b1 << 8) + b2);
        *dy = with_sign(flag >> 1, (b3 << 8) + b4);
    }
    return !g->bad;
}

/* Writes a simple glyph in glyf's own encoding. */
static void write_simple(Out *o, int ncont, const uint16_t *ends, int npts, const int *xs, const int *ys,
                         const uint8_t *oncurve, const uint8_t *instr, uint16_t ilen, const int16_t *bbox,
                         bool overlap)
{
    out_u16(o, (uint32_t)ncont);
    for (int i = 0; i < 4; i++)
        out_u16(o, (uint16_t)bbox[i]);
    for (int i = 0; i < ncont; i++)
        out_u16(o, ends[i]);
    out_u16(o, ilen);
    uint8_t *q = out_room(o, ilen);
    if (q && ilen)
        memcpy(q, instr, ilen);
    /* Flags, with repeats. */
    int px = 0, py = 0;
    uint8_t last = 0;
    int run = -1;
    size_t last_at = 0;
    for (int i = 0; i < npts; i++) {
        int dx = xs[i] - px, dy = ys[i] - py;
        uint8_t fl = oncurve[i] ? 1 : 0;
        if (i == 0 && overlap)
            fl |= 0x40;
        if (dx == 0)
            fl |= 0x10;
        else if (dx >= -255 && dx <= 255)
            fl |= 0x02 | (dx > 0 ? 0x10 : 0);
        if (dy == 0)
            fl |= 0x20;
        else if (dy >= -255 && dy <= 255)
            fl |= 0x04 | (dy > 0 ? 0x20 : 0);
        if (run >= 0 && fl == last && run < 255) {
            run++;
            if (run == 1) {
                /* turn the previous flag into a repeated one */
                o->p[last_at] |= 0x08;
                out_u8(o, 1);
            } else if (!o->bad) {
                o->p[o->len - 1] = (uint8_t)run;
            }
        } else {
            last_at = o->len;
            out_u8(o, fl);
            last = fl;
            run = 0;
        }
        px = xs[i];
        py = ys[i];
    }
    for (int pass = 0; pass < 2; pass++) {
        int prev = 0;
        for (int i = 0; i < npts; i++) {
            int v = pass ? ys[i] : xs[i], d = v - prev;
            if (d != 0) {
                if (d >= -255 && d <= 255)
                    out_u8(o, (uint32_t)(d < 0 ? -d : d));
                else
                    out_u16(o, (uint16_t)(int16_t)d);
            }
            prev = v;
        }
    }
}

/* The length of composite glyph data at r (components until the last),
 * and whether it has instructions. */
static size_t composite_len(In *c, bool *instr)
{
    size_t start = c->at;
    *instr = false;
    for (int n = 0; n < 64; n++) {
        uint16_t flags = in_u16(c);
        in_u16(c); /* glyph */
        size_t argsz = (flags & 1) ? 4 : 2;
        size_t scale = (flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0;
        if (c->bad || argsz + scale > c->len - c->at) {
            c->bad = true;
            return 0;
        }
        c->at += argsz + scale;
        if (flags & 0x100)
            *instr = true;
        if (!(flags & 0x20))
            return c->at - start;
    }
    c->bad = true;
    return 0;
}

/* Rebuilds glyf and loca from the transformed glyf stream. */
static bool rebuild_glyf(const uint8_t *s, size_t slen, size_t max_out, Table *glyf, Table *loca,
                         uint16_t *num_glyphs, int16_t **xmins)
{
    In h = {s, slen, 0, false};
    in_u16(&h); /* reserved */
    uint16_t options = in_u16(&h), ng = in_u16(&h), index_fmt = in_u16(&h);
    uint32_t sizes[7];
    for (int i = 0; i < 7; i++)
        sizes[i] = in_u32(&h);
    if (h.bad || index_fmt > 1)
        return false;
    In st[7];
    size_t at = h.at;
    for (int i = 0; i < 7; i++) {
        if (sizes[i] > slen - at)
            return false;
        st[i].p = s + at;
        st[i].len = sizes[i];
        st[i].at = 0;
        st[i].bad = false;
        at += sizes[i];
    }
    In *ncont_s = &st[0], *npts_s = &st[1], *flag_s = &st[2], *glyph_s = &st[3], *comp_s = &st[4];
    In *bbox_s = &st[5], *instr_s = &st[6];
    /* The bbox stream starts with a bitmap of glyphs with explicit boxes. */
    size_t bm_len = 4 * (((size_t)ng + 31) / 32);
    if (bm_len > bbox_s->len)
        return false;
    const uint8_t *bbox_bm = bbox_s->p;
    bbox_s->at = bm_len;
    const uint8_t *overlap_bm = NULL;
    if (options & 1) {
        if ((ng + 7) / 8 > slen - at)
            return false;
        overlap_bm = s + at;
    }
    Out o = {NULL, 0, 0, max_out, false};
    uint32_t *offs = au_alloc(((size_t)ng + 1) * sizeof *offs);
    int16_t *xm = au_calloc(((size_t)ng ? ng : 1) * sizeof *xm);
    uint16_t *ends = au_alloc(65536 * sizeof *ends);
    int *xs = au_alloc(MAX_GLYPH_POINTS * 2 * sizeof *xs);
    int *ys = xs + MAX_GLYPH_POINTS;
    uint8_t *onc = au_alloc(MAX_GLYPH_POINTS);
    bool ok = false;
    for (uint32_t gi = 0; gi < ng; gi++) {
        offs[gi] = (uint32_t)o.len;
        int16_t nc = (int16_t)in_u16(ncont_s);
        bool has_bbox = (bbox_bm[gi >> 3] & (0x80 >> (gi & 7))) != 0;
        if (ncont_s->bad)
            goto out;
        if (nc == 0) {
            if (has_bbox)
                goto out;
            continue;
        }
        int16_t bbox[4] = {0, 0, 0, 0};
        if (has_bbox)
            for (int k = 0; k < 4; k++)
                bbox[k] = (int16_t)in_u16(bbox_s);
        if (nc > 0) {
            int npts = 0;
            for (int c = 0; c < nc; c++) {
                uint16_t n = in_255u16(npts_s);
                if (npts_s->bad || npts + n > MAX_GLYPH_POINTS || (n == 0 && c == 0 && nc == 1 && 0))
                    goto out;
                npts += n;
                if (npts == 0 || npts > 65536)
                    goto out;
                ends[c] = (uint16_t)(npts - 1);
            }
            int x = 0, y = 0;
            for (int i = 0; i < npts; i++) {
                uint8_t fl = in_u8(flag_s);
                int dx, dy;
                if (flag_s->bad || !triplet(glyph_s, fl, &dx, &dy))
                    goto out;
                x += dx;
                y += dy;
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767)
                    goto out;
                xs[i] = x;
                ys[i] = y;
                onc[i] = !(fl & 0x80);
            }
            uint16_t ilen = in_255u16(glyph_s);
            if (glyph_s->bad || ilen > instr_s->len - instr_s->at)
                goto out;
            const uint8_t *ins = instr_s->p + instr_s->at;
            instr_s->at += ilen;
            if (!has_bbox) {
                bbox[0] = bbox[2] = (int16_t)xs[0];
                bbox[1] = bbox[3] = (int16_t)ys[0];
                for (int i = 1; i < npts; i++) {
                    if (xs[i] < bbox[0])
                        bbox[0] = (int16_t)xs[i];
                    if (xs[i] > bbox[2])
                        bbox[2] = (int16_t)xs[i];
                    if (ys[i] < bbox[1])
                        bbox[1] = (int16_t)ys[i];
                    if (ys[i] > bbox[3])
                        bbox[3] = (int16_t)ys[i];
                }
            }
            bool ov = overlap_bm && (overlap_bm[gi >> 3] & (0x80 >> (gi & 7)));
            write_simple(&o, nc, ends, npts, xs, ys, onc, ins, ilen, bbox, ov);
        } else {
            /* Composite: its bbox must be explicit. */
            if (!has_bbox || nc != -1)
                goto out;
            bool instr;
            size_t cstart = comp_s->at;
            size_t clen = composite_len(comp_s, &instr);
            if (comp_s->bad)
                goto out;
            out_u16(&o, 0xFFFF);
            for (int k = 0; k < 4; k++)
                out_u16(&o, (uint16_t)bbox[k]);
            uint8_t *q = out_room(&o, clen);
            if (q)
                memcpy(q, comp_s->p + cstart, clen);
            if (instr) {
                uint16_t ilen = in_255u16(glyph_s);
                if (glyph_s->bad || ilen > instr_s->len - instr_s->at)
                    goto out;
                out_u16(&o, ilen);
                q = out_room(&o, ilen);
                if (q && ilen)
                    memcpy(q, instr_s->p + instr_s->at, ilen);
                instr_s->at += ilen;
            }
        }
        xm[gi] = bbox[0];
        /* Pad to four bytes (long loca), else two. */
        while (!o.bad && (o.len & (index_fmt ? 3 : 1)))
            out_u8(&o, 0);
        if (o.bad || bbox_s->bad)
            goto out;
    }
    offs[ng] = (uint32_t)o.len;
    if (!index_fmt && o.len > 0x1FFFE)
        goto out; /* too large for a short loca */
    size_t loca_len = ((size_t)ng + 1) * (index_fmt ? 4 : 2);
    if (loca_len > max_out)
        goto out;
    loca->data = au_alloc(loca_len);
    loca->len = (uint32_t)loca_len;
    for (uint32_t i = 0; i <= ng; i++) {
        if (index_fmt)
            put32(loca->data + i * 4, offs[i]);
        else
            put16(loca->data + i * 2, offs[i] / 2);
    }
    glyf->data = o.p ? o.p : au_alloc(1);
    glyf->len = (uint32_t)o.len;
    o.p = NULL;
    *num_glyphs = ng;
    *xmins = xm;
    xm = NULL;
    ok = true;
out:
    au_free(o.p);
    au_free(offs);
    au_free(xm);
    au_free(ends);
    au_free(xs);
    au_free(onc);
    return ok;
}

/* Rebuilds hmtx from its transformed form. */
static bool rebuild_hmtx(const uint8_t *s, size_t slen, uint16_t nh, uint16_t ng, const int16_t *xmins, Table *t)
{
    In r = {s, slen, 0, false};
    uint8_t flags = in_u8(&r);
    if ((flags & 0xFC) || !(flags & 3) || nh > ng || !nh)
        return false;
    size_t len = (size_t)nh * 4 + (size_t)(ng - nh) * 2;
    uint8_t *o = au_alloc(len);
    uint16_t *adv = au_alloc((size_t)nh * sizeof *adv);
    for (uint32_t i = 0; i < nh; i++)
        adv[i] = in_u16(&r);
    for (uint32_t i = 0; i < nh; i++) {
        int16_t lsb = (flags & 1) ? xmins[i] : (int16_t)in_u16(&r);
        put16(o + i * 4, adv[i]);
        put16(o + i * 4 + 2, (uint16_t)lsb);
    }
    for (uint32_t i = nh; i < ng; i++) {
        int16_t lsb = (flags & 2) ? xmins[i] : (int16_t)in_u16(&r);
        put16(o + nh * 4 + (i - nh) * 2, (uint16_t)lsb);
    }
    au_free(adv);
    if (r.bad) {
        au_free(o);
        return false;
    }
    t->data = o;
    t->len = (uint32_t)len;
    return true;
}

/* Decompresses the whole brotli stream into exactly want bytes. */
static bool brotli_exact(const uint8_t *in, size_t in_len, size_t want, uint8_t **out)
{
    AuBrotliDict *dict = au_brotli_dict_new();
    AuBrotli *b = au_brotli_new(dict, 0);
    /* One byte of spare room detects a stream longer than declared. */
    uint8_t *buf = au_alloc(want + 1);
    uint8_t *o = buf;
    size_t olen = want + 1;
    const uint8_t *p = in;
    size_t plen = in_len;
    AuCodecStatus st;
    for (;;) {
        size_t before_in = plen, before_out = olen;
        st = au_brotli_run(b, &p, &plen, &o, &olen);
        if (st != AU_CODEC_OK || (plen == before_in && olen == before_out))
            break; /* done, corrupt, or stalled (truncated input) */
    }
    au_brotli_free(b);
    au_brotli_dict_free(dict);
    if (st != AU_CODEC_DONE || olen != 1) {
        au_free(buf);
        return false;
    }
    *out = buf;
    return true;
}

static AuFontStatus decode_woff2(const uint8_t *d, size_t len, size_t max_out, uint8_t **out, size_t *out_len)
{
    In r = {d, len, 0, false};
    if (len < 48 || in_u32(&r) != AU_TAG('w', 'O', 'F', '2'))
        return AU_FONT_BAD;
    uint32_t flavor = in_u32(&r), length = in_u32(&r);
    uint16_t n = in_u16(&r);
    in_u16(&r);
    in_u32(&r); /* totalSfntSize: our own sizes are checked instead */
    uint32_t comp_size = in_u32(&r);
    r.at = 48;
    if (flavor == AU_TAG('O', 'T', 'T', 'O'))
        return AU_FONT_UNSUPPORTED;
    if (length != len || !n || n > MAX_TABLES)
        return AU_FONT_BAD;
    W2Entry *e = au_calloc(n * sizeof *e);
    Table *t = au_calloc(n * sizeof *t);
    uint16_t **fonts = NULL, *nft = NULL;
    uint32_t *flavors = NULL;
    size_t nfonts = 0;
    uint8_t *stream = NULL;
    int16_t *xmins = NULL;
    AuFontStatus st = AU_FONT_BAD;
    uint64_t total = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t fl = in_u8(&r);
        uint32_t tag = (fl & 63) == 63 ? in_u32(&r) : known_tags[fl & 63];
        uint8_t xv = (uint8_t)(fl >> 6);
        e[i].tag = tag;
        e[i].xform = xv;
        e[i].orig_len = in_base128(&r);
        if (is_cff(tag)) {
            st = AU_FONT_UNSUPPORTED;
            goto out;
        }
        bool glyf_loca = tag == TAG_GLYF || tag == TAG_LOCA;
        e[i].transformed = glyf_loca ? xv == 0 : xv != 0;
        if (e[i].transformed && !(glyf_loca || (tag == TAG_HMTX && xv == 1)))
            goto out; /* an unknown transform */
        e[i].stream_len = e[i].transformed ? in_base128(&r) : e[i].orig_len;
        if (r.bad || e[i].orig_len > max_out)
            goto out;
        if (tag == TAG_LOCA && e[i].transformed && e[i].stream_len != 0)
            goto out;
        e[i].stream_off = (uint32_t)total;
        total += e[i].stream_len;
        if (total > max_out)
            goto out;
    }
    if (flavor == AU_TAG('t', 't', 'c', 'f')) {
        uint32_t ver = in_u32(&r);
        if (ver != 0x00010000 && ver != 0x00020000)
            goto out;
        nfonts = in_255u16(&r);
        if (!nfonts || nfonts > MAX_FONTS || r.bad)
            goto out;
        fonts = au_calloc(nfonts * sizeof *fonts);
        nft = au_calloc(nfonts * sizeof *nft);
        flavors = au_calloc(nfonts * sizeof *flavors);
        for (size_t f = 0; f < nfonts; f++) {
            uint16_t k = in_255u16(&r);
            flavors[f] = in_u32(&r);
            if (r.bad || !k || k > n)
                goto out;
            nft[f] = k;
            fonts[f] = au_alloc(k * sizeof **fonts);
            for (uint16_t j = 0; j < k; j++) {
                uint16_t ti = in_255u16(&r);
                if (r.bad || ti >= n)
                    goto out;
                fonts[f][j] = ti;
            }
        }
    } else {
        nfonts = 1;
        fonts = au_calloc(sizeof *fonts);
        nft = au_calloc(sizeof *nft);
        flavors = au_calloc(sizeof *flavors);
        fonts[0] = au_alloc(n * sizeof **fonts);
        for (uint16_t i = 0; i < n; i++)
            fonts[0][i] = i;
        nft[0] = n;
        flavors[0] = flavor;
    }
    if (r.bad || comp_size > len - r.at)
        goto out;
    if (!brotli_exact(d + r.at, comp_size, (size_t)total, &stream))
        goto out;
    /* Tables, glyf/loca/hmtx rebuilt. A collection may share one glyf
     * between fonts; each table is decoded once. */
    for (uint16_t i = 0; i < n; i++) {
        t[i].tag = e[i].tag;
        if (e[i].transformed)
            continue;
        t[i].data = au_alloc(e[i].orig_len ? e[i].orig_len : 1);
        memcpy(t[i].data, stream + e[i].stream_off, e[i].orig_len);
        t[i].len = e[i].orig_len;
    }
    for (uint16_t i = 0; i < n; i++) {
        if (!e[i].transformed || e[i].tag != TAG_GLYF)
            continue;
        /* The loca of the same font: in a collection, the font that holds
         * this glyf. */
        int loca = -1;
        for (size_t f = 0; f < nfonts && loca < 0; f++) {
            bool has = false;
            for (uint16_t j = 0; j < nft[f]; j++)
                has |= fonts[f][j] == i;
            for (uint16_t j = 0; has && j < nft[f]; j++)
                if (e[fonts[f][j]].tag == TAG_LOCA)
                    loca = fonts[f][j];
        }
        if (loca < 0 || !e[loca].transformed || t[loca].data)
            goto out;
        uint16_t ng = 0;
        au_free(xmins);
        xmins = NULL;
        if (!rebuild_glyf(stream + e[i].stream_off, e[i].stream_len, max_out, &t[i], &t[loca], &ng, &xmins))
            goto out;
        /* hmtx of the font(s) using this glyf. */
        for (size_t f = 0; f < nfonts; f++) {
            bool has = false;
            int hmtx = -1, hhea = -1;
            for (uint16_t j = 0; j < nft[f]; j++) {
                uint16_t ti = fonts[f][j];
                has |= ti == i;
                if (e[ti].tag == TAG_HMTX)
                    hmtx = ti;
                if (e[ti].tag == TAG_HHEA)
                    hhea = ti;
            }
            if (!has || hmtx < 0 || !e[hmtx].transformed || t[hmtx].data)
                continue;
            if (hhea < 0 || t[hhea].len < 36)
                goto out;
            uint16_t nh = (uint16_t)((t[hhea].data[34] << 8) | t[hhea].data[35]);
            if (!rebuild_hmtx(stream + e[hmtx].stream_off, e[hmtx].stream_len, nh, ng, xmins, &t[hmtx]))
                goto out;
        }
    }
    for (uint16_t i = 0; i < n; i++)
        if (!t[i].data)
            goto out; /* a transformed table without what it needs */
    st = assemble(t, n, fonts, nft, flavors, nfonts, max_out, out, out_len);
out:
    au_free(xmins);
    au_free(stream);
    for (size_t f = 0; fonts && f < nfonts; f++)
        au_free(fonts[f]);
    au_free(fonts);
    au_free(nft);
    au_free(flavors);
    au_free(e);
    tables_free(t, n);
    return st;
}

AuFontStatus au_font_decode(const void *data, size_t len, size_t max_out, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    switch (au_font_sniff(data, len)) {
    case AU_FONT_FORMAT_WOFF: return decode_woff(data, len, max_out, out, out_len);
    case AU_FONT_FORMAT_WOFF2: return decode_woff2(data, len, max_out, out, out_len);
    case AU_FONT_FORMAT_CFF: return AU_FONT_UNSUPPORTED;
    case AU_FONT_FORMAT_SFNT:
        if (len > max_out)
            return AU_FONT_BAD;
        *out = au_alloc(len);
        memcpy(*out, data, len);
        *out_len = len;
        return AU_FONT_OK;
    default: return AU_FONT_BAD;
    }
}
