/* The profile folder and the assets (docs/gtk.md, "Files" and "Assets").
 *
 * The profile is $XDG_CONFIG_HOME/aura (~/.config/aura), through the
 * POSIX AuAppFiles. Its Cache and Script Cache folders go to $XDG_CACHE_HOME/aura
 * (~/.cache/aura/Cache) instead, so that the cache sits where Linux tools
 * expect throwaway data; every other name stays in the profile. */
#include "gtk.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

AuGtk au_gtk;

static AuPosixFiles config_files, cache_files;
static AuAppFiles config_ops, cache_ops;

static const AuAppFiles *route(const char *folder)
{
    return folder && (!strcmp(folder, AU_APP_FOLDER_CACHE) || !strcmp(folder, AU_APP_FOLDER_SCRIPT_CACHE)) ? &cache_ops
                                                                                                           : &config_ops;
}

static bool f_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    const AuAppFiles *f = route(folder);
    return f->read(f->ctx, folder, name, out, max);
}

static bool f_write(void *ctx, const char *folder, const char *name, const void *data, size_t len)
{
    const AuAppFiles *f = route(folder);
    return f->write(f->ctx, folder, name, data, len);
}

static bool f_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    const AuAppFiles *f = route(folder);
    return f->rename(f->ctx, folder, name, new_name);
}

static void f_remove(void *ctx, const char *folder, const char *name)
{
    const AuAppFiles *f = route(folder);
    f->remove(f->ctx, folder, name);
}

static bool f_exists(void *ctx, const char *folder, const char *name)
{
    const AuAppFiles *f = route(folder);
    return f->exists(f->ctx, folder, name);
}

static bool f_make_folder(void *ctx, const char *folder)
{
    const AuAppFiles *f = route(folder);
    return f->make_folder(f->ctx, folder);
}

static void f_list(void *ctx, const char *folder, void (*each)(void *each_ctx, const char *name, size_t size, uint32_t mtime),
                   void *each_ctx)
{
    const AuAppFiles *f = route(folder);
    f->list(f->ctx, folder, each, each_ctx);
}

bool au_gtk_files_init(AuAppPort *out)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/aura", g_get_user_config_dir());
    if (!au_posix_files_init(&config_files, path)) {
        fprintf(stderr, "aura: cannot use the profile folder %s\n", path);
        return false;
    }
    snprintf(path, sizeof path, "%s/aura", g_get_user_cache_dir());
    if (!au_posix_files_init(&cache_files, path))
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
 * assets beside the binary, ../share/aura beside the binary (an install
 * tree), the source tree the binary was built from, and the install
 * prefix's data folder. */

static bool try_assets(const char *dir)
{
    if (!dir || !*dir)
        return false;
    char idx[1200];
    snprintf(idx, sizeof idx, "%s/fonts/fonts.idx", dir);
    if (access(idx, R_OK) != 0)
        return false;
    snprintf(au_gtk.assets, sizeof au_gtk.assets, "%s", dir);
    return true;
}

void au_gtk_find_assets(const char *argv0)
{
    au_gtk.assets[0] = 0;
    if (try_assets(g_getenv("AURA_ASSETS")))
        return;
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char dir[1100];
            snprintf(dir, sizeof dir, "%s/assets", exe);
            if (try_assets(dir))
                return;
            snprintf(dir, sizeof dir, "%s/../share/aura", exe);
            if (try_assets(dir))
                return;
        }
    }
    (void)argv0;
    if (try_assets(AURA_SOURCE_ASSETS))
        return;
    if (try_assets(AURA_DATADIR))
        return;
    fprintf(stderr, "aura: no assets folder found (set AURA_ASSETS); using the system's fonts only\n");
}

bool au_gtk_asset_path(const char *rel, char *out, size_t cap)
{
    if (!au_gtk.assets[0])
        return false;
    int n = snprintf(out, cap, "%s/%s", au_gtk.assets, rel);
    return n > 0 && (size_t)n < cap;
}
