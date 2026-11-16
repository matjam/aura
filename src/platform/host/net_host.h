/* The network session of the aura-host tool (net_host.c): the profile
 * folder where it keeps its cookie and HSTS files between runs, the root
 * store, and the sockets of src/platform/posix (AuPosixNet). Also the
 * entry points of the tool's network commands. */
#ifndef AURA_HOST_NET_H
#define AURA_HOST_NET_H

#include "browser/app.h"
#include "net/net.h"
#include "platform/posix/posix.h"

/* The trust anchors from assets/net/aura-roots.bin (path fixed at build
 * time, or $AURA_ROOTS). NULL with a message on stderr on failure. */
AuRootStore *au_host_load_roots(void);

/* The application services' port (browser/app.h) over the profile
 * folder, $AURA_PROFILE, else ~/.aura-host (made when missing):
 * posix.h's au_posix_app_files and au_posix_app_port on that folder. */
void au_host_app_files(AuAppFiles *out);
void au_host_app_port(AuAppPort *out);

/* Everything a host command needs to make requests: sockets, the root
 * store, the AuNet, and the cookie jar, HSTS store and intermediate
 * certificates loaded from the profile folder by browser/app.c (files
 * "Cookies", "HSTS" and "Intermediates", as Mac OS 9 names them). */
typedef struct AuHostSession {
    AuPosixNet *host;
    AuRootStore *roots;
    AuNet *net;
    AuCookieJar *jar; /* NULL with cookies off */
    AuHsts *hsts;
} AuHostSession;
bool au_host_session_open(AuHostSession *s, bool cookies, bool verbose);
/* Saves the stores when they changed, and frees everything. */
void au_host_session_close(AuHostSession *s);

/* The web storage service (net/web_storage.h) for a host tool, following
 * jar's cookie rules (NULL: none). With persist its files are kept in
 * the folder "Local Storage" of the profile folder (browser/app.c's
 * AuAppFolder); without, it lives in memory. Close saves what changed
 * and frees it (after the browser). */
struct AuWebStorage;
struct AuWebStorage *au_host_storage_open(AuCookieJar *jar, bool persist);
void au_host_storage_close(struct AuWebStorage *ws);

/* aura-host fetch URL [-o file] [-v] ... (argv[1] is "fetch"). */
int au_host_fetch_main(int argc, char **argv);
/* aura-host render|dump with an http or https URL (net_render.c). */
bool au_host_net_render_wanted(int argc, char **argv);
int au_host_net_render_main(int argc, char **argv);

/* aura-host blocklist ... (blocklist_tool.c; argv[1] is "blocklist"). */
int au_host_blocklist_main(int argc, char **argv);
/* For --blocklist FILE: a set holding the compiled or text list at path,
 * or NULL with a message; and its counts, printed after a run. */
struct AuBlocklist;
struct AuBlocklist *au_host_blocklist_open(const char *path);
void au_host_blocklist_report(struct AuBlocklist *bl);

#endif
