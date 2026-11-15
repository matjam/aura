/* Reading the installer's archive; sea_archive.h describes it. */
#include "sea_archive.h"

#include <string.h>

enum { HEADER = 48, ENTRY = 52 };

static const char sea_magic[] = "AuraSEA1";

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int sea_read(AuSea *sea, void *to, uint32_t n)
{
    int err;
    if (n > sea->left)
        return AU_SEA_DAMAGED;
    err = sea->io.read(sea->io.ctx, to, n);
    if (err == 0)
        sea->left -= n;
    return err;
}

int au_sea_trailer(const unsigned char trailer[AU_SEA_TRAILER], uint32_t fork_len, uint32_t *offset,
                   uint32_t *length, uint32_t *header_crc)
{
    *offset = be32(trailer + 8);
    *length = be32(trailer + 12);
    *header_crc = be32(trailer + 16);
    if (memcmp(trailer, sea_magic, 8) != 0 || fork_len < AU_SEA_TRAILER + HEADER || *length < HEADER ||
        *offset > fork_len - AU_SEA_TRAILER || *length != fork_len - AU_SEA_TRAILER - *offset)
        return AU_SEA_DAMAGED;
    return 0;
}

int au_sea_begin(AuSea *sea, const AuSeaIo *io, uint32_t length, uint32_t header_crc, AuInflate *inflate,
                 unsigned char *in, unsigned char *out, unsigned char folder[32], uint32_t *entries,
                 uint32_t *total)
{
    unsigned char b[HEADER];
    int err;
    sea->io = *io;
    sea->left = length;
    sea->entries = 0;
    sea->inflate = inflate;
    sea->in = in;
    sea->out = out;
    err = sea_read(sea, b, HEADER);
    if (err != 0)
        return err;
    if (memcmp(b, sea_magic, 8) != 0 || au_codec_crc32(0, b, HEADER) != header_crc || b[16] == 0 || b[16] > 31)
        return AU_SEA_DAMAGED;
    sea->entries = *entries = be32(b + 8);
    *total = be32(b + 12);
    memcpy(folder, b + 16, 32);
    return 0;
}

int au_sea_next(AuSea *sea, AuSeaEntry *e)
{
    unsigned char b[ENTRY + AU_SEA_MAX_PATH + 4];
    uint32_t n, i;
    int err;
    if (sea->entries == 0)
        return AU_SEA_DAMAGED;
    err = sea_read(sea, b, ENTRY);
    if (err != 0)
        return err;
    n = (uint32_t)b[6] << 8 | b[7];
    if (be32(b) != 0x4175456EUL /* 'AuEn' */ || b[4] > 1 || n == 0 || n > AU_SEA_MAX_PATH)
        return AU_SEA_DAMAGED;
    err = sea_read(sea, b + ENTRY, n + 4);
    if (err != 0)
        return err;
    if (au_codec_crc32(0, b, ENTRY + n) != be32(b + ENTRY + n))
        return AU_SEA_DAMAGED;
    e->kind = b[4];
    e->type = be32(b + 8);
    e->creator = be32(b + 12);
    e->flags = (uint16_t)(b[16] << 8 | b[17]);
    e->created = be32(b + 20);
    e->modified = be32(b + 24);
    e->data_len = be32(b + 28);
    e->data_clen = be32(b + 32);
    e->data_crc = be32(b + 36);
    e->rsrc_len = be32(b + 40);
    e->rsrc_clen = be32(b + 44);
    e->rsrc_crc = be32(b + 48);
    /* A leading colon makes the path a partial path name. An empty
     * component would name the parent folder ("::"), and a component
     * longer than 31 bytes is not an HFS name. */
    e->path[0] = (unsigned char)(n + 1);
    e->path[1] = ':';
    memcpy(e->path + 2, b + ENTRY, n);
    for (i = 2; i <= n + 1; i++) {
        uint32_t start = i;
        while (i <= n + 1 && e->path[i] != ':')
            i++;
        if (i == start || i - start > 31 || i == n + 1)
            return AU_SEA_DAMAGED;
    }
    if (e->kind == 0 && (e->data_len | e->data_clen | e->rsrc_len | e->rsrc_clen) != 0)
        return AU_SEA_DAMAGED;
    sea->entries--;
    return 0;
}

int au_sea_fork(AuSea *sea, uint32_t len, uint32_t clen, uint32_t want_crc)
{
    uint32_t crc = 0, total = 0;
    const uint8_t *in = sea->in;
    size_t in_len = 0;
    AuCodecStatus st = AU_CODEC_OK;
    int err;

    if (clen == 0)
        return len == 0 ? 0 : AU_SEA_DAMAGED;
    au_inflate_reset(sea->inflate);
    while (st != AU_CODEC_DONE) {
        uint8_t *o = sea->out;
        size_t out_len = AU_SEA_OUT;
        size_t had = in_len;
        uint32_t made;
        if (in_len == 0 && clen != 0) {
            uint32_t n = clen < AU_SEA_IN ? clen : AU_SEA_IN;
            err = sea_read(sea, sea->in, n);
            if (err != 0)
                return err;
            clen -= n;
            in = sea->in;
            had = in_len = n;
        }
        /* With no input left the decoder still has output to give, or
         * reports the end of the stream. */
        st = au_inflate_run(sea->inflate, &in, &in_len, &o, &out_len);
        if (st == AU_CODEC_ERROR)
            return AU_SEA_DAMAGED;
        made = (uint32_t)(AU_SEA_OUT - out_len);
        if (made > len - total)
            return AU_SEA_DAMAGED;
        if (made != 0) {
            total += made;
            crc = au_codec_crc32(crc, sea->out, made);
            err = sea->io.write(sea->io.ctx, sea->out, made);
            if (err != 0)
                return err;
        } else if (st == AU_CODEC_OK && in_len == had && (had != 0 || clen == 0)) {
            return AU_SEA_DAMAGED; /* no progress: the stream is cut short */
        }
    }
    if (clen != 0 || in_len != 0 || total != len || crc != want_crc)
        return AU_SEA_DAMAGED;
    return 0;
}

int au_sea_end(const AuSea *sea) { return sea->left == 0 && sea->entries == 0 ? 0 : AU_SEA_DAMAGED; }
