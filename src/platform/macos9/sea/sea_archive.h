/* The archive inside the Aura installer: reading it.
 *
 * Portable C with no OS calls, so that the host tests run the same code
 * the installer runs on Mac OS 9 (tests/tools). The caller supplies the
 * bytes and takes the output through AuSeaIo.
 *
 * The archive (big-endian; tools/make-sea.py writes it):
 *
 *   header   "AuraSEA1", u32 entry count, u32 total bytes of all forks,
 *            Pascal string in 32 bytes: the folder's name
 *   entry    u32 'AuEn', u8 kind (0 folder, 1 file), u8 0, u16 path length,
 *            type, creator, u16 Finder flags, u16 0,
 *            u32 creation date, u32 modification date (Mac OS seconds),
 *            data fork: u32 length, u32 compressed length, u32 CRC-32,
 *            resource fork: the same three,
 *            the path (Mac Roman, components separated by ':', relative
 *            to the folder), u32 CRC-32 of the entry up to here,
 *            the data fork, then the resource fork, each as a raw deflate
 *            stream (nothing for an empty fork)
 *   trailer  the last 20 bytes of the installer's data fork: "AuraSEA1",
 *            u32 offset of the header in the fork, u32 length of the
 *            archive from the header to the trailer, u32 CRC-32 of the
 *            header
 *
 * To read one: pass the fork's last 20 bytes to au_sea_trailer, position
 * the input at the offset it returns, call au_sea_begin, then for each
 * entry au_sea_next and, for a file, au_sea_fork for the data fork and
 * again for the resource fork, and au_sea_end after the last entry. */
#ifndef AURA_SEA_ARCHIVE_H
#define AURA_SEA_ARCHIVE_H

#include "codec/codec.h"

enum {
    AU_SEA_DAMAGED = 1, /* the archive is not what the packer wrote */

    AU_SEA_TRAILER = 20,
    AU_SEA_MAX_PATH = 240,
    AU_SEA_IN = 16 * 1024,
    AU_SEA_OUT = 32 * 1024
};

/* Each function returns 0, AU_SEA_DAMAGED, or the nonzero value a callback
 * returned (an OS error, or the caller's own code for a stop). */
typedef struct {
    void *ctx;
    /* Reads exactly n bytes of the archive, in order. */
    int (*read)(void *ctx, void *to, uint32_t n);
    /* Writes n bytes to the fork being extracted. */
    int (*write)(void *ctx, const void *from, uint32_t n);
} AuSeaIo;

typedef struct {
    unsigned char kind; /* 0 folder, 1 file */
    uint32_t type, creator;
    uint16_t flags;
    uint32_t created, modified;
    uint32_t data_len, data_clen, data_crc;
    uint32_t rsrc_len, rsrc_clen, rsrc_crc;
    /* A Pascal string: ":Fonts:inter:Inter.ttf", a partial path name
     * relative to the folder. No component is empty. */
    unsigned char path[AU_SEA_MAX_PATH + 2];
} AuSeaEntry;

typedef struct {
    AuSeaIo io;
    uint32_t left;    /* archive bytes not yet read */
    uint32_t entries; /* entries not yet read */
    AuInflate *inflate;
    unsigned char *in, *out; /* AU_SEA_IN and AU_SEA_OUT bytes */
} AuSea;

/* Checks the trailer of a data fork of fork_len bytes and returns where
 * the archive starts, its length and the CRC its header must have. */
int au_sea_trailer(const unsigned char trailer[AU_SEA_TRAILER], uint32_t fork_len, uint32_t *offset,
                   uint32_t *length, uint32_t *header_crc);

/* Reads the header. `in` and `out` are the caller's buffers of AU_SEA_IN
 * and AU_SEA_OUT bytes; `inflate` is a raw inflater (AU_INFLATE_RAW).
 * Returns the folder's name (a Pascal string of at most 31 characters),
 * the number of entries and the total size of the forks. */
int au_sea_begin(AuSea *sea, const AuSeaIo *io, uint32_t length, uint32_t header_crc, AuInflate *inflate,
                 unsigned char *in, unsigned char *out, unsigned char folder[32], uint32_t *entries,
                 uint32_t *total);

int au_sea_next(AuSea *sea, AuSeaEntry *e);

/* Inflates one fork to io.write and checks its length and CRC. */
int au_sea_fork(AuSea *sea, uint32_t len, uint32_t clen, uint32_t crc);

/* After the last entry: nothing may be left over. */
int au_sea_end(const AuSea *sea);

#endif
