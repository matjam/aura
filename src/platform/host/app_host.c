/* The application services' port on the host (browser/app.h): the files
 * of the profile folder ($AURA_PROFILE, else ~/.aura-host) and its
 * subfolders over POSIX files, and the system clocks. See net_host.h. */
#include "net_host.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The path of a file of the profile folder or of one of its folders. */
static bool path_of(const char *folder, const char *name, char *out, size_t cap)
{
    char dir[4096];
    if (!folder)
        return au_host_profile_path(name, out, cap);
    if (!au_host_profile_path(folder, dir, sizeof dir))
        return false;
    if (!name)
        return snprintf(out, cap, "%s", dir) < (int)cap;
    return snprintf(out, cap, "%s/%s", dir, name) < (int)cap;
}

static bool h_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    (void)ctx;
    char path[4200];
    struct stat st;
    if (!path_of(folder, name, path, sizeof path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (unsigned long long)st.st_size >= (unsigned long long)max)
        return false;
    size_t len;
    char *data = au_host_read_file(path, &len);
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
    (void)ctx;
    char path[4200];
    return path_of(folder, name, path, sizeof path) && au_host_write_file(path, data, len);
}

static bool h_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    (void)ctx;
    char from[4200], to[4200];
    if (!path_of(folder, name, from, sizeof from) || !path_of(folder, new_name, to, sizeof to))
        return false;
    fprintf(stderr, "aura-host: %s did not parse; renamed \"%s\"\n", from, new_name);
    return rename(from, to) == 0;
}

static void h_remove(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    char path[4200];
    if (path_of(folder, name, path, sizeof path))
        remove(path);
}

static bool h_exists(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    char path[4200];
    struct stat st;
    return path_of(folder, name, path, sizeof path) && stat(path, &st) == 0;
}

static bool h_make_folder(void *ctx, const char *folder)
{
    (void)ctx;
    char path[4200];
    struct stat st;
    if (!path_of(folder, NULL, path, sizeof path))
        return false;
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return false;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void h_list(void *ctx, const char *folder, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    (void)ctx;
    char dir[4200];
    if (!path_of(folder, NULL, dir, sizeof dir))
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

void au_host_app_files(AuAppFiles *out)
{
    memset(out, 0, sizeof *out);
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static uint32_t c_us(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

static int64_t c_wall(void *ctx)
{
    (void)ctx;
    return (int64_t)time(NULL);
}

static int32_t c_offset(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    struct tm lt, ut;
    if (!localtime_r(&t, &lt) || !gmtime_r(&t, &ut))
        return 0;
    /* The difference of the two broken-down times, in seconds. */
    long d = (long)(lt.tm_hour - ut.tm_hour) * 3600 + (long)(lt.tm_min - ut.tm_min) * 60;
    int day = (lt.tm_year != ut.tm_year) ? (lt.tm_year > ut.tm_year ? 1 : -1) : (lt.tm_yday - ut.tm_yday);
    return (int32_t)(d + (long)day * 86400);
}

void au_host_app_port(AuAppPort *out)
{
    memset(out, 0, sizeof *out);
    au_host_app_files(&out->files);
    out->ms = c_ms;
    out->us = c_us;
    out->wall_time = c_wall;
    out->utc_offset = c_offset;
    /* No wall_ms: script keeps the C library's clock and zone. */
}
