/* Extracts the archive of an Aura installer on the host, with the code
 * the installer runs on Mac OS 9 (src/platform/macos9/sea/sea_archive.c).
 *
 * Usage: sea_extract INSTALLER.bin DESTFOLDER
 *
 * Reads the MacBinary file, finds the archive at the end of its data
 * fork and writes each file's data fork to DESTFOLDER/path and its
 * resource fork, when it has one, to DESTFOLDER/path.rsrc. Prints one line
 * per entry. Exits 1 when the archive is damaged. tests/tools/test_sea.py
 * compares the result with what tools/make-sea.py packed. */
#include "platform/macos9/sea/sea_archive.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const unsigned char *data;
    size_t len, pos;
    FILE *out;
} Ctx;

static int io_read(void *ctx, void *to, uint32_t n)
{
    Ctx *c = ctx;
    if (n > c->len - c->pos)
        return AU_SEA_DAMAGED;
    memcpy(to, c->data + c->pos, n);
    c->pos += n;
    return 0;
}

static int io_write(void *ctx, const void *from, uint32_t n)
{
    Ctx *c = ctx;
    return fwrite(from, 1, n, c->out) == n ? 0 : -36;
}

static int fail(const char *what, const char *path)
{
    fprintf(stderr, "sea_extract: %s%s%s\n", what, path ? ": " : "", path ? path : "");
    return 1;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int put_fork(AuSea *sea, Ctx *c, const char *path, uint32_t len, uint32_t clen, uint32_t crc)
{
    int err;
    c->out = fopen(path, "wb");
    if (!c->out)
        return fail("cannot create", path);
    err = au_sea_fork(sea, len, clen, crc);
    if (fclose(c->out) != 0 && err == 0)
        err = -36;
    return err;
}

int main(int argc, char **argv)
{
    static unsigned char in[AU_SEA_IN], out[AU_SEA_OUT];
    static const unsigned char no_file[1] = {0};
    AuSeaIo io = {NULL, io_read, io_write};
    AuSea sea;
    AuSeaEntry e;
    Ctx c;
    unsigned char *file = NULL, folder[32];
    size_t size = 0, cap = 0, got;
    uint32_t dlen, offset, length, crc, entries, total, i;
    FILE *f;
    int err;

    if (argc != 3) {
        fprintf(stderr, "usage: sea_extract INSTALLER.bin DESTFOLDER\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
        return fail("cannot open", argv[1]);
    do {
        cap = cap ? cap * 2 : 1 << 20;
        file = realloc(file, cap);
        if (!file)
            return fail("out of memory", NULL);
        got = fread(file + size, 1, cap - size, f);
        size += got;
    } while (size == cap);
    fclose(f);

    /* MacBinary: a 128-byte header with the data fork's length at 83. */
    if (size < 128 || file[0] != 0 || file[74] != 0)
        return fail("not a MacBinary file", argv[1]);
    dlen = be32(file + 83);
    if (dlen > size - 128 || dlen < AU_SEA_TRAILER)
        return fail("damaged", "the data fork is cut short");
    c.data = dlen ? file + 128 : no_file;
    c.len = dlen;
    c.out = NULL;
    io.ctx = &c;

    err = au_sea_trailer(c.data + dlen - AU_SEA_TRAILER, dlen, &offset, &length, &crc);
    if (err == 0) {
        c.pos = offset;
        c.len = offset + length;
        err = au_sea_begin(&sea, &io, length, crc, au_inflate_new(AU_INFLATE_RAW), in, out, folder, &entries,
                           &total);
    }
    if (err != 0)
        return fail("damaged", "trailer or header");
    printf("folder %.*s entries %lu total %lu\n", (int)folder[0], (const char *)folder + 1, (unsigned long)entries,
           (unsigned long)total);
    if (mkdir(argv[2], 0777) != 0 && errno != EEXIST)
        return fail("cannot create", argv[2]);

    for (i = 0; i < entries; i++) {
        char path[2048];
        size_t base, k;
        err = au_sea_next(&sea, &e);
        if (err != 0)
            return fail("damaged", "an entry");
        base = strlen(argv[2]);
        if (base + e.path[0] + 8 > sizeof path)
            return fail("path too long", argv[2]);
        memcpy(path, argv[2], base);
        for (k = 1; k <= e.path[0]; k++) /* the leading colon becomes the first slash */
            path[base + k - 1] = e.path[k] == ':' ? '/' : e.path[k] == '/' ? ':' : (char)e.path[k];
        path[base + e.path[0]] = 0;
        printf("%d %.*s %08lx %08lx %04x %lu %lu\n", e.kind, (int)e.path[0] - 1, (const char *)e.path + 2,
               (unsigned long)e.type, (unsigned long)e.creator, e.flags, (unsigned long)e.data_len,
               (unsigned long)e.rsrc_len);
        if (e.kind == 0) {
            if (mkdir(path, 0777) != 0)
                return fail("cannot create", path);
            continue;
        }
        err = put_fork(&sea, &c, path, e.data_len, e.data_clen, e.data_crc);
        if (err == 0 && e.rsrc_len != 0) {
            strcat(path, ".rsrc");
            err = put_fork(&sea, &c, path, e.rsrc_len, e.rsrc_clen, e.rsrc_crc);
        } else if (err == 0 && e.rsrc_clen != 0) {
            err = AU_SEA_DAMAGED;
        }
        if (err != 0)
            return fail("damaged", path);
    }
    if (au_sea_end(&sea) != 0)
        return fail("damaged", "bytes left after the last entry");
    free(file);
    return 0;
}
