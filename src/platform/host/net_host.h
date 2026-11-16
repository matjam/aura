/* The host's AuNetOps: BSD sockets (non-blocking), getaddrinfo on a
 * helper thread per lookup, the system random generator, and the system
 * clocks. Also the host profile folder where aura-host keeps its cookie
 * and HSTS files between runs. */
#ifndef AURA_HOST_NET_H
#define AURA_HOST_NET_H

#include "net/net.h"

typedef struct AuHostNet AuHostNet;

AuHostNet *au_host_net_new(void);
void au_host_net_free(AuHostNet *h); /* closes every socket it opened */
const AuNetOps *au_host_net_ops(AuHostNet *h);
/* Waits up to ms for socket activity or a finished lookup. */
void au_host_net_wait(AuHostNet *h, int ms);

/* The trust anchors from assets/net/aura-roots.bin (path fixed at build
 * time, or $AURA_ROOTS). NULL with a message on stderr on failure. */
AuRootStore *au_host_load_roots(void);

/* The profile folder: $AURA_PROFILE, else ~/.aura-host. Created if
 * missing. Returns false when it cannot be made. */
bool au_host_profile_path(const char *name, char *out, size_t cap);
/* Reads a whole file (NUL-terminated, au_free); NULL if absent. */
char *au_host_read_file(const char *path, size_t *len);
/* Writes a file safely: to path.tmp, then renamed over path. */
bool au_host_write_file(const char *path, const void *data, size_t len);

/* Everything a host command needs to make requests: sockets, the root
 * store, the AuNet, and the cookie jar and HSTS store loaded from the
 * profile folder (files "cookies" and "hsts"). */
typedef struct AuHostSession {
    AuHostNet *host;
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
 * the folder "local-storage" of the profile folder; without, it lives in
 * memory. Close saves what changed and frees it (after the browser). */
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
