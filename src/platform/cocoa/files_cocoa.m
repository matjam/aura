/* The profile folder and the assets (docs/cocoa.md, "Profile and assets").
 *
 * The profile is ~/Library/Application Support/Aura, through the POSIX
 * AuAppFiles. Its Cache folder goes to ~/Library/Caches/Aura/Cache
 * instead, where macOS expects data it may throw away; every other name
 * stays in the profile. */
#import "cocoa.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AuPosixFiles config_files, cache_files;
static AuAppFiles config_ops, cache_ops;

static const AuAppFiles *route(const char *folder)
{
    return folder && !strcmp(folder, AU_APP_FOLDER_CACHE) ? &cache_ops : &config_ops;
}

static bool f_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    return f->read(f->ctx, folder, name, out, max);
}

static bool f_write(void *ctx, const char *folder, const char *name, const void *data, size_t len)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    return f->write(f->ctx, folder, name, data, len);
}

static bool f_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    return f->rename(f->ctx, folder, name, new_name);
}

static void f_remove(void *ctx, const char *folder, const char *name)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    f->remove(f->ctx, folder, name);
}

static bool f_exists(void *ctx, const char *folder, const char *name)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    return f->exists(f->ctx, folder, name);
}

static bool f_make_folder(void *ctx, const char *folder)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    return f->make_folder(f->ctx, folder);
}

static void f_list(void *ctx, const char *folder,
                   void (*each)(void *each_ctx, const char *name, size_t size, uint32_t mtime), void *each_ctx)
{
    const AuAppFiles *f = route(folder);
    (void)ctx;
    f->list(f->ctx, folder, each, each_ctx);
}

static NSString *library_folder(NSSearchPathDirectory which)
{
    NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(which, NSUserDomainMask, YES);
    return dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Library"];
}

bool au_cocoa_files_init(AuAppPort *out)
{
    NSString *profile = getenv("AURA_PROFILE")
                            ? au_cocoa_str(getenv("AURA_PROFILE"))
                            : [library_folder(NSApplicationSupportDirectory) stringByAppendingPathComponent:@"Aura"];
    if (!au_posix_files_init(&config_files, profile.fileSystemRepresentation)) {
        fprintf(stderr, "aura: cannot use the profile folder %s\n", profile.UTF8String);
        return false;
    }
    NSString *cache = getenv("AURA_PROFILE")
                          ? profile
                          : [library_folder(NSCachesDirectory) stringByAppendingPathComponent:@"Aura"];
    if (!au_posix_files_init(&cache_files, cache.fileSystemRepresentation))
        cache_files = config_files; /* the cache in the profile is better than none */
    au_posix_app_files(&config_files, &config_ops);
    au_posix_app_files(&cache_files, &cache_ops);
    au_posix_app_port(&config_files, out);
    out->files.ctx = NULL;
    out->files.read = f_read;
    out->files.write = f_write;
    out->files.rename = f_rename;
    out->files.remove = f_remove;
    out->files.exists = f_exists;
    out->files.make_folder = f_make_folder;
    out->files.list = f_list;
    return true;
}

/* ---- Assets ----
 * The first folder holding fonts/fonts.idx, in this order: $AURA_ASSETS,
 * the bundle's Resources/assets, and the source tree the application was
 * built from. */

static bool try_assets(const char *dir)
{
    if (!dir || !*dir)
        return false;
    char idx[1200];
    snprintf(idx, sizeof idx, "%s/fonts/fonts.idx", dir);
    if (access(idx, R_OK) != 0)
        return false;
    snprintf(au_cocoa.assets, sizeof au_cocoa.assets, "%s", dir);
    return true;
}

void au_cocoa_find_assets(void)
{
    au_cocoa.assets[0] = 0;
    if (try_assets(getenv("AURA_ASSETS")))
        return;
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"assets"];
    if (res && try_assets(res.fileSystemRepresentation))
        return;
    if (try_assets(AURA_SOURCE_ASSETS))
        return;
    fprintf(stderr, "aura: no assets folder found (set AURA_ASSETS); using the system's fonts only\n");
}

bool au_cocoa_asset_path(const char *rel, char *out, size_t cap)
{
    if (!au_cocoa.assets[0])
        return false;
    int n = snprintf(out, cap, "%s/%s", au_cocoa.assets, rel);
    return n > 0 && (size_t)n < cap;
}
