/* Files over POSIX (see posix.h): whole-file reads, safe writes, and the
 * application services' files (browser/app.h) over a profile folder and
 * its subfolders, with the clocks of clock_posix.c. */
#include "posix.h"
#include "browser/app.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool au_platform_read_file(const char *path, char **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    /* A file whose size is known is read into a block of that size: a
     * buffer grown by doubling keeps up to twice the bytes, and a font's
     * data is kept for good. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        rewind(f);
        if (size > 0) {
            char *d = au_alloc((size_t)size + 1);
            size_t got = fread(d, 1, (size_t)size, f);
            int extra = fgetc(f);
            if (got == (size_t)size && extra == EOF && !ferror(f)) {
                fclose(f);
                d[got] = 0;
                *data = d;
                *len = got;
                return true;
            }
            /* It changed while being read: read it as a stream. */
            au_free(d);
            rewind(f);
        }
    }
    AuBuf b;
    au_buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        au_buf_append(&b, chunk, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        au_buf_free(&b);
        return false;
    }
    *data = au_buf_take(&b, len);
    return true;
}

char *au_posix_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    AuBuf b;
    au_buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        au_buf_append(&b, tmp, n);
    fclose(f);
    return au_buf_take(&b, len);
}

bool au_posix_write_file(const char *path, const void *data, size_t len)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool au_posix_make_dirs(const char *path)
{
    char p[2048];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof p)
        return false;
    memcpy(p, path, n + 1);
    /* Each parent in turn, then the folder itself. */
    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0)
            continue;
        char c = p[i];
        p[i] = 0;
        if (mkdir(p, 0700) != 0 && errno != EEXIST) {
            p[i] = c;
            return false;
        }
        p[i] = c;
    }
    return is_dir(path);
}

bool au_posix_files_init(AuPosixFiles *f, const char *root)
{
    if (snprintf(f->root, sizeof f->root, "%s", root) >= (int)sizeof f->root) {
        f->root[0] = 0;
        return false;
    }
    return au_posix_make_dirs(f->root);
}

/* The path of a file of the profile folder or of one of its folders
 * (name NULL: the folder itself). */
static bool path_of(const AuPosixFiles *f, const char *folder, const char *name, char *out, size_t cap)
{
    if (!f->root[0])
        return false;
    if (!folder && !name)
        return snprintf(out, cap, "%s", f->root) < (int)cap;
    if (!folder)
        return snprintf(out, cap, "%s/%s", f->root, name) < (int)cap;
    if (!name)
        return snprintf(out, cap, "%s/%s", f->root, folder) < (int)cap;
    return snprintf(out, cap, "%s/%s/%s", f->root, folder, name) < (int)cap;
}

static bool h_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    char path[4200];
    struct stat st;
    if (!path_of(ctx, folder, name, path, sizeof path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (unsigned long long)st.st_size >= (unsigned long long)max)
        return false;
    size_t len;
    char *data = au_posix_read_file(path, &len);
    if (!data)
        return false;
    bool ok = len < max;
    if (ok)
        au_buf_append(out, data, len);
    au_free(data);
    return ok;
}

static bool h_write(void *ctx, const char *folder, const char *name, const void *data, size_t len)
{
    char path[4200];
    return path_of(ctx, folder, name, path, sizeof path) && au_posix_write_file(path, data, len);
}

static bool h_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    char from[4200], to[4200];
    if (!path_of(ctx, folder, name, from, sizeof from) || !path_of(ctx, folder, new_name, to, sizeof to))
        return false;
    fprintf(stderr, "aura: %s did not parse; renamed \"%s\"\n", from, new_name);
    return rename(from, to) == 0;
}

static void h_remove(void *ctx, const char *folder, const char *name)
{
    char path[4200];
    if (path_of(ctx, folder, name, path, sizeof path))
        remove(path);
}

static bool h_exists(void *ctx, const char *folder, const char *name)
{
    char path[4200];
    struct stat st;
    return path_of(ctx, folder, name, path, sizeof path) && stat(path, &st) == 0;
}

static bool h_make_folder(void *ctx, const char *folder)
{
    char path[4200];
    if (!path_of(ctx, folder, NULL, path, sizeof path))
        return false;
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return false;
    return is_dir(path);
}

static void h_list(void *ctx, const char *folder, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    char dir[4200];
    if (!path_of(ctx, folder, NULL, dir, sizeof dir))
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[4500];
        struct stat st;
        size_t nl = strlen(e->d_name);
        if (e->d_name[0] == '.' || snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (nl > 4 && !strcmp(e->d_name + nl - 4, ".tmp")) {
            remove(path); /* an interrupted write */
            continue;
        }
        each(ec, e->d_name, (size_t)st.st_size, (uint32_t)st.st_mtime);
    }
    closedir(d);
}

void au_posix_app_files(AuPosixFiles *f, AuAppFiles *out)
{
    memset(out, 0, sizeof *out);
    out->ctx = f;
    out->read = h_read;
    out->write = h_write;
    out->rename = h_rename;
    out->remove = h_remove;
    out->exists = h_exists;
    out->make_folder = h_make_folder;
    out->list = h_list;
}

static uint32_t c_ms(void *ctx)
{
    (void)ctx;
    return au_posix_ms();
}

static uint32_t c_us(void *ctx)
{
    (void)ctx;
    return au_posix_us();
}

static int64_t c_wall(void *ctx)
{
    (void)ctx;
    return au_posix_wall_time();
}

static int32_t c_offset(void *ctx)
{
    (void)ctx;
    return au_posix_utc_offset();
}

void au_posix_app_port(AuPosixFiles *f, AuAppPort *out)
{
    memset(out, 0, sizeof *out);
    au_posix_app_files(f, &out->files);
    out->ms = c_ms;
    out->us = c_us;
    out->wall_time = c_wall;
    out->utc_offset = c_offset;
    /* No wall_ms: script keeps the C library's clock and zone. */
}
