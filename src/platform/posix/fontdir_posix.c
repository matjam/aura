/* Font files on disk (see posix.h): single files read whole, and the faces
 * of a Fonts folder (assets/fonts in the source tree, the Fonts folder
 * beside a port's application), registered from its fonts.idx and loaded
 * on first use. Without an index the folder is scanned and each font read
 * once to describe it. */
#include "posix.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct FaceFile {
    struct FaceFile *next;
    AuPosixFontDir *dir;
    char path[1024];
    char *data;
    size_t len;
    bool tried;
    FILE *fp; /* open while a paged face reads from it */
} FaceFile;

struct AuPosixFontDir {
    char root[1024];
    FaceFile *files;
    AuFontSet *set;
    unsigned flags;
    int added;
    size_t loads;
    size_t reads; /* reads of paged faces */
};

static bool load_file(void *ctx, const void **data, size_t *len)
{
    FaceFile *f = ctx;
    if (!f->tried) {
        f->tried = true;
        if (au_platform_read_file(f->path, &f->data, &f->len))
            f->dir->loads++;
        else
            f->data = NULL;
    }
    if (!f->data)
        return false;
    *data = f->data;
    *len = f->len;
    return true;
}

/* Large files are paged: the set asks for the size, then reads pieces. */
static bool file_size(void *ctx, uint32_t *len)
{
    FaceFile *f = ctx;
    struct stat st;
    if (stat(f->path, &st) != 0 || st.st_size < 0 || (unsigned long long)st.st_size > 0x7FFFFFFFu)
        return false;
    *len = (uint32_t)st.st_size;
    return true;
}

static bool file_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    FaceFile *f = ctx;
    if (!f->fp) {
        f->fp = fopen(f->path, "rb");
        if (!f->fp)
            return false;
        f->dir->loads++;
    }
    if (fseek(f->fp, (long)off, SEEK_SET) != 0)
        return false;
    f->dir->reads++;
    return fread(buf, 1, n, f->fp) == n;
}

static const AuFontFileOps file_ops = {load_file, file_size, file_read};

/* One FaceFile per path: faces of one .ttc share it. */
static FaceFile *file_for(AuPosixFontDir *d, const char *path)
{
    for (FaceFile *f = d->files; f; f = f->next)
        if (!strcmp(f->path, path))
            return f;
    FaceFile *f = au_calloc(sizeof *f);
    f->dir = d;
    snprintf(f->path, sizeof f->path, "%s", path);
    f->next = d->files;
    d->files = f;
    return f;
}

static void add_entry(void *ctx, const AuFontIndexEntry *e)
{
    AuPosixFontDir *d = ctx;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", d->root, e->path) >= (int)sizeof path)
        return;
    au_posix_font_dir_add(d, path, e->family, e->weight, e->italic, e->face_index);
}

AuPosixFontDir *au_posix_font_dir_new(AuFontSet *set, unsigned flags)
{
    AuPosixFontDir *d = au_calloc(sizeof *d);
    d->set = set;
    d->flags = flags;
    return d;
}

void au_posix_font_dir_add(AuPosixFontDir *d, const char *path, const char *family, int weight, bool italic,
                           int face_index)
{
    if (strlen(path) >= sizeof d->files->path)
        return;
    FaceFile *f = file_for(d, path);
    au_fontset_add_file(d->set, family, weight, italic, face_index, d->flags, &file_ops, f);
    d->added++;
}

static bool has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && au_ieq(s + n - m, m, suf, m);
}

/* Describes the fonts under dir (prefix: their path relative to the root)
 * into b, as index lines, two folder levels deep. */
static void scan(AuBuf *b, const char *dir, const char *prefix, int depth)
{
    struct dirent **list;
    int nlist = scandir(dir, &list, NULL, alphasort); /* sorted: the index is reproducible */
    if (nlist < 0)
        return;
    for (int li = 0; li < nlist; li++) {
        struct dirent *de = list[li];
        if (de->d_name[0] == '.')
            continue;
        char full[1024], rel[512];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        snprintf(rel, sizeof rel, "%s%s", prefix, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) {
                char sub[512];
                if (snprintf(sub, sizeof sub, "%s/", rel) < (int)sizeof sub)
                    scan(b, full, sub, depth + 1);
            }
            continue;
        }
        if (!has_suffix(de->d_name, ".ttf") && !has_suffix(de->d_name, ".ttc"))
            continue;
        char *data;
        size_t len;
        if (au_platform_read_file(full, &data, &len)) {
            au_font_index_write(b, rel, data, len);
            au_free(data);
        }
    }
    for (int li = 0; li < nlist; li++)
        free(list[li]);
    free(list);
}

bool au_posix_font_index(const char *dir, AuBuf *out)
{
    au_buf_printf(out, "# Aura font index: path, face, weight, style, family. Written by aura-host font-index.\n");
    size_t before = out->len;
    scan(out, dir, "", 0);
    return out->len > before;
}

bool au_posix_add_font_file(AuFontSet *set, const char *path)
{
    char *data;
    size_t len;
    if (!au_platform_read_file(path, &data, &len))
        return false; /* missing: the caller decides whether to say so */
    /* The data stays allocated for the life of the program. */
    if (au_fontset_add_data(set, data, len) == 0) {
        fprintf(stderr, "aura: %s: not a usable TrueType font\n", path);
        au_free(data);
        return false;
    }
    return true;
}

AuPosixFontDir *au_posix_add_font_dir(AuFontSet *set, const char *dir, unsigned flags)
{
    AuPosixFontDir *d = au_posix_font_dir_new(set, flags);
    snprintf(d->root, sizeof d->root, "%s", dir);
    char path[1100];
    snprintf(path, sizeof path, "%s/fonts.idx", dir);
    char *idx;
    size_t len;
    if (au_platform_read_file(path, &idx, &len)) {
        au_font_index_parse(idx, len, add_entry, d);
        au_free(idx);
    } else {
        AuBuf b;
        au_buf_init(&b);
        au_posix_font_index(dir, &b);
        au_font_index_parse(b.data ? b.data : "", b.len, add_entry, d);
        au_buf_free(&b);
    }
    return d;
}

int au_posix_font_dir_count(const AuPosixFontDir *d)
{
    return d ? d->added : 0;
}

size_t au_posix_font_dir_loads(const AuPosixFontDir *d)
{
    return d ? d->loads : 0;
}

void au_posix_font_dir_free(AuPosixFontDir *d)
{
    if (!d)
        return;
    FaceFile *f = d->files;
    while (f) {
        FaceFile *n = f->next;
        if (f->fp)
            fclose(f->fp);
        au_free(f->data);
        au_free(f);
        f = n;
    }
    au_free(d);
}
