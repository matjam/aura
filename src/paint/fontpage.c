/* Paged faces: a font file whose glyph outlines stay in the file.
 *
 * The pager reads the face's table directory and every table but glyf
 * into one compact sfnt in memory (cmap, metrics, loca, names: about 5% of
 * a CJK face), and reads glyf in blocks of AU_FONT_PAGE_SIZE bytes when a
 * glyph is drawn, keeping the AU_FONT_PAGE_SLOTS most recently used
 * blocks. Rasterized glyphs are cached by the glyph cache, so a block is
 * read again only when the glyph cache has dropped every glyph in it.
 *
 * Every size comes from the file and is checked before use: table ranges
 * against the file length, the table count and the total of the tables
 * kept against fixed limits. */
#include "font_internal.h"

#include <string.h>

#define MAX_TABLES 256
#define MAX_KEPT (16u * 1024 * 1024) /* the tables held in memory */

typedef struct PageSlot {
    uint32_t block; /* block number + 1; 0 for an empty slot */
    uint32_t used;  /* the pager's clock at the last use */
    uint32_t n;     /* valid bytes (the last block is short) */
    uint8_t *data;  /* AU_FONT_PAGE_SIZE bytes, allocated on first use */
} PageSlot;

struct AuFontPager {
    AuFontReadFn read;
    void *ctx;
    uint8_t *data; /* the compact sfnt */
    uint32_t len;
    uint32_t glyf_off, glyf_len; /* glyf in the file */
    uint32_t clock;
    PageSlot slot[AU_FONT_PAGE_SLOTS];
    size_t reads, hits;
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool in_file(uint32_t file_len, uint32_t off, uint32_t n)
{
    return off <= file_len && n <= file_len - off;
}

AuFontPager *au_font_pager_new(AuFontReadFn read, void *ctx, uint32_t file_len, int index)
{
    uint8_t head[16];
    if (!read || file_len < 12 || file_len > 0x7FFFFFFF || index < 0 || !read(ctx, 0, head, 12))
        return NULL;
    uint32_t dir = 0;
    if (be32(head) == AU_TAG('t', 't', 'c', 'f')) {
        uint8_t b[4];
        if (be32(head + 8) > 1024 || (uint32_t)index >= be32(head + 8) ||
            !in_file(file_len, 12 + (uint32_t)index * 4, 4) || !read(ctx, 12 + (uint32_t)index * 4, b, 4))
            return NULL;
        dir = be32(b);
        if (!in_file(file_len, dir, 12) || !read(ctx, dir, head, 12))
            return NULL;
    } else if (index != 0) {
        return NULL;
    }
    uint32_t version = be32(head);
    if (version != 0x00010000u && version != AU_TAG('t', 'r', 'u', 'e'))
        return NULL; /* not TrueType outlines (CFF has no glyf to page) */
    uint32_t ntables = (uint32_t)((head[4] << 8) | head[5]);
    if (!ntables || ntables > MAX_TABLES || !in_file(file_len, dir + 12, ntables * 16))
        return NULL;
    uint8_t *recs = au_alloc(ntables * 16);
    AuFontPager *p = NULL;
    if (!read(ctx, dir + 12, recs, ntables * 16))
        goto out;
    /* Sizes: the directory, then each kept table padded to 4 bytes. */
    uint32_t glyf_off = 0, glyf_len = 0, kept = 0, size = 12 + ntables * 16;
    bool have_glyf = false;
    for (uint32_t i = 0; i < ntables; i++) {
        const uint8_t *r = recs + i * 16;
        uint32_t tag = be32(r), off = be32(r + 8), n = be32(r + 12);
        if (!in_file(file_len, off, n))
            continue; /* past the end: absent, as au_font_face_new treats it */
        if (tag == AU_TAG('g', 'l', 'y', 'f')) {
            if (have_glyf)
                continue;
            have_glyf = true;
            glyf_off = off;
            glyf_len = n;
            continue;
        }
        uint32_t padded = (n + 3) & ~3u;
        if (n > MAX_KEPT || padded > MAX_KEPT - kept)
            goto out;
        kept += padded;
    }
    if (!have_glyf || !glyf_len)
        goto out;
    p = au_calloc(sizeof *p);
    p->read = read;
    p->ctx = ctx;
    p->glyf_off = glyf_off;
    p->glyf_len = glyf_len;
    p->len = size + kept;
    p->data = au_calloc(p->len);
    memcpy(p->data, head, 12);
    /* The compact font is one face whatever the file was. */
    uint32_t at = size, nout = 0;
    have_glyf = false;
    for (uint32_t i = 0; i < ntables; i++) {
        const uint8_t *r = recs + i * 16;
        uint32_t tag = be32(r), off = be32(r + 8), n = be32(r + 12);
        if (!in_file(file_len, off, n))
            continue;
        uint8_t *o = p->data + 12 + nout * 16;
        if (tag == AU_TAG('g', 'l', 'y', 'f')) {
            if (have_glyf)
                continue;
            have_glyf = true;
            memcpy(o, r, 4);
            put32(o + 8, 0); /* the parser takes glyf from the pager */
            put32(o + 12, n);
            nout++;
            continue;
        }
        if (n && !read(ctx, off, p->data + at, n)) {
            au_font_pager_free(p);
            p = NULL;
            goto out;
        }
        memcpy(o, r, 4);
        put32(o + 8, at);
        put32(o + 12, n);
        at += (n + 3) & ~3u;
        nout++;
    }
    p->data[4] = (uint8_t)(nout >> 8);
    p->data[5] = (uint8_t)nout;
out:
    au_free(recs);
    return p;
}

void au_font_pager_free(AuFontPager *p)
{
    if (!p)
        return;
    for (int i = 0; i < AU_FONT_PAGE_SLOTS; i++)
        au_free(p->slot[i].data);
    au_free(p->data);
    au_free(p);
}

AuFontFace *au_font_face_new_paged(AuFontPager *p)
{
    return p ? au_font_face_parse(p->data, p->len, 0, p) : NULL;
}

uint32_t au_font_pager_glyf_len(const AuFontPager *p)
{
    return p->glyf_len;
}

void au_font_pager_stats(const AuFontPager *p, AuFontPagerStats *st)
{
    memset(st, 0, sizeof *st);
    if (!p)
        return;
    st->table_bytes = p->len;
    for (int i = 0; i < AU_FONT_PAGE_SLOTS; i++)
        if (p->slot[i].data)
            st->block_bytes += AU_FONT_PAGE_SIZE;
    st->reads = p->reads;
    st->hits = p->hits;
}

/* The cached block b of glyf, read from the file on a miss; NULL when it
 * cannot be read. */
static PageSlot *block(AuFontPager *p, uint32_t b)
{
    PageSlot *victim = &p->slot[0];
    p->clock++;
    for (int i = 0; i < AU_FONT_PAGE_SLOTS; i++) {
        PageSlot *s = &p->slot[i];
        if (s->block == b + 1) {
            s->used = p->clock;
            p->hits++;
            return s;
        }
        if (!s->block) {
            if (victim->block)
                victim = s;
        } else if (victim->block && s->used < victim->used) {
            victim = s;
        }
    }
    uint32_t start = b * AU_FONT_PAGE_SIZE;
    uint32_t n = p->glyf_len - start < AU_FONT_PAGE_SIZE ? p->glyf_len - start : AU_FONT_PAGE_SIZE;
    if (!victim->data)
        victim->data = au_alloc(AU_FONT_PAGE_SIZE);
    victim->block = 0;
    if (!p->read(p->ctx, p->glyf_off + start, victim->data, n))
        return NULL;
    victim->block = b + 1;
    victim->n = n;
    victim->used = p->clock;
    p->reads++;
    return victim;
}

const uint8_t *au_font_pager_glyf(AuFontPager *p, uint32_t off, uint32_t n, uint8_t **own)
{
    *own = NULL;
    if (!n || n > AU_FONT_GLYPH_MAX || off > p->glyf_len || n > p->glyf_len - off)
        return NULL;
    uint32_t first = off / AU_FONT_PAGE_SIZE, last = (off + n - 1) / AU_FONT_PAGE_SIZE;
    if (first == last) {
        PageSlot *s = block(p, first);
        return s ? s->data + (off - first * AU_FONT_PAGE_SIZE) : NULL;
    }
    uint8_t *buf = au_alloc(n);
    uint32_t done = 0;
    for (uint32_t b = first; b <= last; b++) {
        PageSlot *s = block(p, b);
        if (!s) {
            au_free(buf);
            return NULL;
        }
        uint32_t from = b == first ? off - b * AU_FONT_PAGE_SIZE : 0;
        uint32_t take = s->n - from;
        if (take > n - done)
            take = n - done;
        memcpy(buf + done, s->data + from, take);
        done += take;
    }
    if (done != n) {
        au_free(buf);
        return NULL;
    }
    *own = buf;
    return buf;
}
