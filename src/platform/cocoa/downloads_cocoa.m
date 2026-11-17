/* Download files (AuDownloadFiles for src/net/transfer.c), in the download
 * folder: the preference, else ~/Downloads.
 *
 * A download is written to "<id> <name>.part", so a paused or interrupted
 * download finds its bytes again after a restart. When it completes, the
 * partial file is renamed to the final name, made unique in the folder,
 * and marked as downloaded from the web (com.apple.quarantine), so the
 * system asks before it runs anything in it. Nothing is ever opened
 * automatically. */
#import "cocoa.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>

typedef struct DlFile {
    FILE *fp;
} DlFile;

void au_cocoa_download_folder(char *out, size_t cap)
{
    const char *pref = au_cocoa.browser ? au_cocoa.browser->prefs.download_folder : NULL;
    if (pref && *pref) {
        snprintf(out, cap, "%s", pref);
        return;
    }
    NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    NSString *d = dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"];
    snprintf(out, cap, "%s", d.fileSystemRepresentation);
}

static void partial_path(const AuDownload *d, char *out, size_t cap)
{
    char dir[1024];
    au_cocoa_download_folder(dir, sizeof dir);
    snprintf(out, cap, "%s/%lu %s.part", dir, (unsigned long)d->id, d->filename ? d->filename : "download");
}

static char *err_text(const char *what, int err)
{
    char buf[256];
    if (err == ENOSPC)
        snprintf(buf, sizeof buf, "The disk is full");
    else if (err == EACCES || err == EPERM || err == EROFS)
        snprintf(buf, sizeof buf, "%s: permission denied", what);
    else
        snprintf(buf, sizeof buf, "%s (%s)", what, strerror(err));
    return au_strndup(buf, strlen(buf));
}

static void *f_open(void *ctx, AuDownload *d, bool restart, int64_t *existing, char **error)
{
    char dir[1024], path[1400];
    (void)ctx;
    au_cocoa_download_folder(dir, sizeof dir);
    if (!au_posix_make_dirs(dir)) {
        *error = err_text("Cannot make the download folder", errno);
        return NULL;
    }
    partial_path(d, path, sizeof path);
    FILE *fp = fopen(path, restart ? "wb" : "ab");
    if (!fp) {
        *error = err_text("Cannot open the file", errno);
        return NULL;
    }
    long pos = 0;
    if (fseek(fp, 0, SEEK_END) == 0)
        pos = ftell(fp);
    DlFile *f = au_calloc(sizeof *f);
    f->fp = fp;
    *existing = pos > 0 ? pos : 0;
    return f;
}

static bool f_write(void *ctx, void *file, const void *bytes, size_t len, char **error)
{
    DlFile *f = file;
    (void)ctx;
    if (fwrite(bytes, 1, len, f->fp) != len) {
        *error = err_text("Cannot write the file", errno);
        return false;
    }
    return true;
}

static bool f_has_space(void *ctx, void *file, int64_t bytes)
{
    char dir[1024];
    struct statfs st;
    (void)ctx;
    (void)file;
    au_cocoa_download_folder(dir, sizeof dir);
    if (statfs(dir, &st) != 0)
        return true; /* unknown: let the writes find out */
    return (int64_t)st.f_bavail * (int64_t)st.f_bsize >= bytes + 64 * 1024;
}

static bool name_taken(void *ctx, const char *name)
{
    char path[1400];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", (const char *)ctx, name);
    return stat(path, &st) == 0;
}

/* Gatekeeper's mark: downloaded by Aura, now. */
static void quarantine(const char *path)
{
    char v[128];
    snprintf(v, sizeof v, "0081;%08lx;Aura;", (unsigned long)time(NULL));
    setxattr(path, "com.apple.quarantine", v, strlen(v), 0, 0);
}

static bool f_close(void *ctx, void *file, AuDownload *d, bool complete, char **error)
{
    DlFile *f = file;
    (void)ctx;
    int err = fclose(f->fp) != 0 ? errno : 0;
    au_free(f);
    if (!complete)
        return true;
    if (err) {
        if (error)
            *error = err_text("Cannot finish the file", err);
        return false;
    }
    char dir[1024], part[1400], done[1400], unique[128];
    au_cocoa_download_folder(dir, sizeof dir);
    partial_path(d, part, sizeof part);
    au_hfs_sanitize(d->filename, name_taken, dir, unique, sizeof unique);
    snprintf(done, sizeof done, "%s/%s", dir, unique);
    if (rename(part, done) != 0) {
        if (error)
            *error = err_text("Cannot rename the file", errno);
        return false;
    }
    quarantine(done);
    if (strcmp(unique, d->filename)) {
        au_free(d->filename);
        d->filename = au_strndup(unique, strlen(unique));
    }
    return true;
}

static void f_remove(void *ctx, AuDownload *d)
{
    char path[1400];
    (void)ctx;
    if (!d->filename || !*d->filename)
        return;
    partial_path(d, path, sizeof path);
    remove(path);
}

void au_cocoa_download_files(AuDownloadFiles *out)
{
    out->ctx = NULL;
    out->open = f_open;
    out->write = f_write;
    out->has_space = f_has_space;
    out->close = f_close;
    out->remove = f_remove;
}

/* Programs and installers, for the warning before such a download. */
bool au_cocoa_download_is_app(const char *name, const char *mime)
{
    static const char *const exts[] = {".app", ".pkg", ".mpkg", ".dmg", ".command", ".sh",  ".tool",
                                       ".exe", ".msi", ".bat",  ".jar", ".scpt",    ".action", ".workflow"};
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot)
        for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
            if (!strcasecmp(dot, exts[i]))
                return true;
    static const char *const types[] = {"application/x-apple-diskimage", "application/vnd.apple.installer+xml",
                                        "application/x-msdownload",      "application/x-sh",
                                        "application/x-executable",      "application/java-archive"};
    if (mime)
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
            if (!strcmp(mime, types[i]))
                return true;
    return false;
}
