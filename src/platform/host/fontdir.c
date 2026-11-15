/* Bundled fonts on the host: the faces of a Fonts folder (assets/fonts in
 * the source tree), registered from its fonts.idx and loaded on first
 * use. Without an index the folder is scanned and each font read once to
 * describe it. */
#include "fontdir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct FaceFile {
    struct FaceFile *next;
    AuHostFontDir *dir;
    char path[1024];
    char *data;
    size_t len;
    bool tried;
} FaceFile;

struct AuHostFontDir {
    char root[1024];
    FaceFile *files;
    AuFontSet *set;
    unsigned flags;
    int added;
    size_t loads;
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

/* One FaceFile per path: faces of one .ttc share it. */
static FaceFile *file_for(AuHostFontDir *d, const char *path)
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
    AuHostFontDir *d = ctx;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", d->root, e->path);
    FaceFile *f = file_for(d, path);
    au_fontset_add_lazy(d->set, e->family, e->weight, e->italic, e->face_index, d->flags, load_file, f);
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
                snprintf(sub, sizeof sub, "%s/", rel);
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

bool au_host_font_index(const char *dir, AuBuf *out)
{
    au_buf_printf(out, "# Aura font index: path, face, weight, style, family. Written by aura-host font-index.\n");
    size_t before = out->len;
    scan(out, dir, "", 0);
    return out->len > before;
}

AuHostFontDir *au_host_add_font_dir(AuFontSet *set, const char *dir, unsigned flags)
{
    AuHostFontDir *d = au_calloc(sizeof *d);
    snprintf(d->root, sizeof d->root, "%s", dir);
    d->set = set;
    d->flags = flags;
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
        au_host_font_index(dir, &b);
        au_font_index_parse(b.data ? b.data : "", b.len, add_entry, d);
        au_buf_free(&b);
    }
    return d;
}

int au_host_font_dir_count(const AuHostFontDir *d)
{
    return d ? d->added : 0;
}

size_t au_host_font_dir_loads(const AuHostFontDir *d)
{
    return d ? d->loads : 0;
}

void au_host_font_dir_free(AuHostFontDir *d)
{
    if (!d)
        return;
    FaceFile *f = d->files;
    while (f) {
        FaceFile *n = f->next;
        au_free(f->data);
        au_free(f);
        f = n;
    }
    au_free(d);
}
