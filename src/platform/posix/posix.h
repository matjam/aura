/* The POSIX services a Unix port of Aura reuses: sockets and DNS for the
 * network stack, files and the application files over a folder, clocks
 * and the time zone, entropy, coroutines, and font files on disk.
 *
 * Nothing here runs an event loop or blocks the caller (au_posix_net_wait
 * excepted, for tools without a main loop). A port with its own main loop
 * (the GLib main loop in the GTK4 port) watches the file descriptors that
 * au_posix_net_watch lists, calls au_posix_net_ready and then au_net_pump
 * when one is ready, and also pumps from a timer for the stack's
 * timeouts. See docs/porting.md, "The POSIX services".
 *
 * The code builds on macOS and Linux (glibc and musl); each call that one
 * of them lacks has a fallback in the file that uses it. */
#ifndef AURA_POSIX_H
#define AURA_POSIX_H

#include "core/coro.h"
#include "platform/platform.h"

/* browser/app.h, which a port includes where it builds the ports; not
 * included here, because it brings the whole browser model with it. */
struct AuAppFiles;
struct AuAppPort;

/* ---- Clocks and time zone (clock_posix.c) ----
 * clock_posix.c also defines au_platform_time_ms. */

/* Milliseconds and microseconds of the monotonic clock; they wrap. */
uint32_t au_posix_ms(void);
uint32_t au_posix_us(void);
/* Seconds since 1970-01-01 00:00 UTC. */
int64_t au_posix_wall_time(void);
/* Seconds east of UTC for the local time zone now (TZ and the system's
 * zone, through localtime_r). */
int32_t au_posix_utc_offset(void);

/* ---- Entropy (entropy_posix.c) ---- */

/* Fills buf with len bytes from the system's generator: getentropy where
 * the C library has it, else /dev/urandom. False when neither works. */
bool au_posix_entropy(void *buf, size_t len);

/* ---- Files (files_posix.c) ----
 * files_posix.c also defines au_platform_read_file. */

/* Reads a whole file (NUL-terminated, au_free); NULL if it cannot be
 * opened. */
char *au_posix_read_file(const char *path, size_t *len);
/* Writes a file safely: to path.tmp, then renamed over path. */
bool au_posix_write_file(const char *path, const void *data, size_t len);
/* Makes the folder path and any missing parents (mode 0700). True when
 * it exists afterwards. */
bool au_posix_make_dirs(const char *path);

/* The application services' files (browser/app.h, AuAppFiles) over a
 * folder: the profile folder is root, and its named folders ("Cache",
 * "Local Storage") are subfolders of it. A port picks root (the host
 * tool: $AURA_PROFILE or ~/.aura-host; GTK4: an XDG folder). */
typedef struct AuPosixFiles {
    char root[2048];
} AuPosixFiles;
/* Sets root and makes it with its parents. False when the path is too
 * long or the folder cannot be made. */
bool au_posix_files_init(AuPosixFiles *f, const char *root);
/* The AuAppFiles over f (f is its ctx and must outlive it). */
void au_posix_app_files(AuPosixFiles *f, struct AuAppFiles *out);
/* The AuAppPort: the files over f, and the clocks above. No wall_ms, so
 * script's Date keeps the C library's clock and zone. */
void au_posix_app_port(AuPosixFiles *f, struct AuAppPort *out);

/* ---- Network (net_posix.c) ----
 * The AuNetOps over non-blocking BSD sockets, getaddrinfo on a helper
 * thread per lookup (it writes a byte to a wake pipe when it finishes),
 * the clocks above and au_posix_entropy. $AURA_FAKE_TIME (seconds since
 * 1970) replaces the wall clock of these ops, to test the stack's clock
 * check. */
typedef struct AuPosixNet AuPosixNet;

AuPosixNet *au_posix_net_new(void);
/* Closes every socket it opened. */
void au_posix_net_free(AuPosixNet *n);
const AuNetOps *au_posix_net_ops(AuPosixNet *n);

/* What to watch a descriptor for. */
enum { AU_POSIX_WATCH_READ = 1, AU_POSIX_WATCH_WRITE = 2 };
typedef struct AuPosixWatch {
    int fd;
    unsigned events; /* AU_POSIX_WATCH_* */
} AuPosixWatch;
/* The most descriptors au_posix_net_watch lists: every socket and the
 * wake pipe. */
#define AU_POSIX_NET_MAX_WATCH 257

/* Lists the descriptors a main loop watches for the stack, up to cap, and
 * returns how many: the wake pipe and every open socket for reading, and
 * for writing too each socket that is still connecting or that last
 * refused part of a send. When one is ready (or reports an error or
 * hang-up), call au_posix_net_ready and then au_net_pump. */
int au_posix_net_watch(AuPosixNet *n, AuPosixWatch *out, int cap);
/* Changes each time the list au_posix_net_watch returns may have changed
 * (a socket opened, closed, finished connecting, or began or stopped
 * waiting to write). A main loop compares it after each pump and lists
 * the descriptors again only when it differs. */
unsigned au_posix_net_watch_serial(AuPosixNet *n);
/* Empties the wake pipe. Call it before pumping when the pipe is ready,
 * or the pipe stays readable. */
void au_posix_net_ready(AuPosixNet *n);
/* For tools without a main loop: waits up to ms for a watched descriptor
 * with poll, then calls au_posix_net_ready. */
void au_posix_net_wait(AuPosixNet *n, int ms);

/* ---- Coroutines (coro_posix.c) ---- */

/* Coroutines (core/coro.h) as threads that run one at a time. */
const AuCoroOps *au_posix_coro_ops(void);

/* ---- Font files (fontdir_posix.c) ---- */

/* Reads a font file whole and adds its faces to the set. The data lives
 * as long as the program. False when the file cannot be read, or (with a
 * message on stderr) when it holds no usable face. */
bool au_posix_add_font_file(AuFontSet *set, const char *path);

typedef struct AuPosixFontDir AuPosixFontDir;

/* Registers the faces of a Fonts folder with the set, lazily: from
 * dir/fonts.idx, or by scanning dir and its subfolders for .ttf and .ttc
 * files when there is no index. flags: AU_FONT_*. The result owns the
 * loaded font data; free it after the set. */
AuPosixFontDir *au_posix_add_font_dir(AuFontSet *set, const char *dir, unsigned flags);
/* Faces registered, and font files read so far. */
int au_posix_font_dir_count(const AuPosixFontDir *d);
size_t au_posix_font_dir_loads(const AuPosixFontDir *d);
void au_posix_font_dir_free(AuPosixFontDir *d);
/* Writes the fonts.idx text for the fonts under dir. False when there are
 * none. */
bool au_posix_font_index(const char *dir, AuBuf *out);

#endif
