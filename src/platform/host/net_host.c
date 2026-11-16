/* The aura-host tool's network session (see net_host.h): the profile
 * folder, the root store, and the stores browser/app.c loads from and
 * saves to the profile folder, over the sockets of src/platform/posix. */
#include "net_host.h"
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/roots.h"
#include "net/web_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- The profile folder ----
 * $AURA_PROFILE, else ~/.aura-host. Web storage's files are in its folder
 * "Local Storage", and the Cookies, HSTS and Intermediates files in it,
 * loaded and saved by the application services as on Mac OS 9 (a file
 * that does not parse is renamed "<name> (damaged)"). */

static AuPosixFiles profile;
static AuAppFiles profile_files;
static AuAppFolder ls_folder;

static AuPosixFiles *profile_folder(void)
{
    if (!profile.root[0]) {
        const char *dir = getenv("AURA_PROFILE");
        char buf[2048];
        if (!dir || !*dir) {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(buf, sizeof buf, "%s/.aura-host", home);
                dir = buf;
            }
        }
        /* Without a folder every file operation fails, as before. */
        if (dir && !au_posix_files_init(&profile, dir))
            fprintf(stderr, "aura-host: cannot use the profile folder %s\n", dir);
    }
    return &profile;
}

void au_host_app_files(AuAppFiles *out)
{
    au_posix_app_files(profile_folder(), out);
}

void au_host_app_port(AuAppPort *out)
{
    au_posix_app_port(profile_folder(), out);
}

static const AuAppFiles *files(void)
{
    if (!profile_files.read)
        au_host_app_files(&profile_files);
    return &profile_files;
}

AuWebStorage *au_host_storage_open(AuCookieJar *jar, bool persist)
{
    AuWebStorage *ws = au_web_storage_new();
    if (jar)
        au_web_storage_set_cookie_jar(ws, jar);
    if (persist && au_app_folder_init(&ls_folder, files(), AU_APP_FOLDER_LOCAL_STORAGE,
                                      (size_t)AU_WEB_STORAGE_FILE_MAX + 1)) {
        AuWebStorageFiles wf;
        au_app_folder_web_storage(&ls_folder, &wf);
        au_web_storage_set_files(ws, &wf);
    }
    return ws;
}

void au_host_storage_close(AuWebStorage *ws)
{
    if (!ws)
        return;
    au_web_storage_save(ws, 0);
    au_web_storage_free(ws);
}

bool au_host_session_open(AuHostSession *s, bool cookies, bool verbose)
{
    memset(s, 0, sizeof *s);
    s->host = au_posix_net_new();
    if (!s->host)
        return false;
    const AuNetOps *ops = au_posix_net_ops(s->host);
    s->roots = au_host_load_roots();
    s->net = au_net_new(ops);
    if (s->roots)
        au_net_set_roots(s->net, s->roots);
    int64_t now = ops->wall_time(ops->ctx);
    if (cookies) {
        s->jar = au_cookie_jar_new();
        au_net_set_cookie_jar(s->net, s->jar);
    }
    s->hsts = au_hsts_new();
    au_net_set_hsts(s->net, s->hsts);
    au_app_load_stores(files(), s->jar, s->hsts, s->net, now);
    if (verbose)
        fprintf(stderr, "* profile: %u cookies, %u HSTS hosts\n", s->jar ? (unsigned)au_cookie_jar_count(s->jar) : 0,
                (unsigned)au_hsts_count(s->hsts));
    return true;
}

void au_host_session_close(AuHostSession *s)
{
    if (!s->host)
        return;
    const AuNetOps *ops = au_posix_net_ops(s->host);
    au_app_save_stores(files(), s->jar, s->hsts, s->net, ops->wall_time(ops->ctx));
    au_net_free(s->net);
    if (s->jar)
        au_cookie_jar_free(s->jar);
    au_hsts_free(s->hsts);
    if (s->roots)
        au_roots_free(s->roots);
    au_posix_net_free(s->host);
    memset(s, 0, sizeof *s);
}

AuRootStore *au_host_load_roots(void)
{
    const char *path = getenv("AURA_ROOTS");
#ifdef AURA_ROOTS_FILE
    if (!path || !*path)
        path = AURA_ROOTS_FILE;
#endif
    if (!path) {
        fprintf(stderr, "aura-host: no root store (set AURA_ROOTS)\n");
        return NULL;
    }
    size_t len;
    char *data = au_posix_read_file(path, &len);
    if (!data) {
        fprintf(stderr, "aura-host: cannot read %s\n", path);
        return NULL;
    }
    char err[128];
    AuRootStore *r = au_roots_load(data, len, err, sizeof err);
    au_free(data);
    if (!r)
        fprintf(stderr, "aura-host: %s: %s\n", path, err);
    return r;
}
