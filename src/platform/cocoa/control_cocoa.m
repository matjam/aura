/* The control socket (src/browser/control.h): with AURA_CONTROL set to a
 * path, the application listens there on a Unix socket. Each connection
 * sends one command line and gets the reply, then the connection closes.
 * The commands run on the main thread, between events, on the front
 * browser window; "shot PATH" writes it to a PNG (au_cocoa_snapshot).
 * tools/aura-ctl.py is a client. Only this user can connect: the socket
 * is made with mode 0600. */
#import "cocoa.h"
#import "windows_cocoa.h"

#include "../../browser/control.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static dispatch_source_t listener;

/* One line from c, at most 64 KB, waiting at most 5 s for it. */
static bool read_line(int c, AuBuf *line)
{
    struct timeval tv = {5, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[4096];
    while (line->len < 65536) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0)
            break;
        au_buf_append(line, buf, (size_t)n);
        if (memchr(buf, '\n', (size_t)n))
            break;
    }
    while (line->len && (line->data[line->len - 1] == '\n' || line->data[line->len - 1] == '\r'))
        line->len--;
    return line->len > 0;
}

static void write_all(int c, const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(c, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return;
        p += k;
        n -= (size_t)k;
    }
}

static void serve(int c)
{
    AuBuf line, out;
    au_buf_init(&line);
    au_buf_init(&out);
    if (read_line(c, &line)) {
        const char *cmd = au_buf_cstr(&line);
        AuCocoaWin *w = au_cocoa_front();
        if (!strcmp(cmd, "headers")) {
            /* record request and response headers from now on, as the
             * Network window does (net details then shows them) */
            if (au_cocoa_wh.record_headers)
                au_cocoa_wh.record_headers(au_cocoa_wh.ctx);
            au_buf_puts(&out, "ok");
        } else if (!strncmp(cmd, "shot ", 5)) {
            au_cocoa_win_flush(w);
            au_buf_puts(&out, w && au_cocoa_snapshot(cmd + 5) ? "ok" : "error no window");
        } else if (!w || w->freed || !w->bw) {
            au_buf_puts(&out, "error no browser window");
        } else {
            au_control_run(w->bw, cmd, &out);
            au_cocoa_kick();
        }
    }
    au_buf_putc(&out, '\n');
    write_all(c, (const char *)out.data, out.len);
    au_buf_free(&line);
    au_buf_free(&out);
    close(c);
}

/* AURA_LOG: every console line of every page and frame, with Trace
 * Network on and the request and response heads recorded, appended to
 * the file as it happens (au_page_set_log). Each line starts with the
 * wall clock time and the level; a line that continues another starts
 * with a tab. */
static FILE *log_file;

static void log_line(void *ctx, int level, const char *text, size_t len)
{
    (void)ctx;
    static bool heads;
    if (!heads && au_cocoa_wh.record_headers) {
        /* the first line comes from a page, so the network stack is up */
        au_cocoa_wh.record_headers(au_cocoa_wh.ctx);
        heads = true;
    }
    static const char *const names[] = {"log", "info", "warn", "error", "debug"};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    fprintf(log_file, "%02d:%02d:%02d.%03d %-5s ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000),
            level >= 0 && level < 5 ? names[level] : "?");
    for (size_t i = 0; i < len; i++) {
        fputc(text[i], log_file);
        if (text[i] == '\n' && i + 1 < len)
            fputc('\t', log_file);
    }
    fputc('\n', log_file);
    fflush(log_file);
}

static void log_start(void)
{
    const char *path = getenv("AURA_LOG");
    if (!path || !*path)
        return;
    log_file = fopen(path, "a");
    if (!log_file) {
        fprintf(stderr, "aura: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    au_page_set_log(log_line, NULL, AU_PAGE_TRACE_NET);
    fprintf(stderr, "aura: log %s\n", path);
}

void au_cocoa_control_start(void)
{
    log_start();
    const char *path = getenv("AURA_CONTROL");
    if (!path || !*path)
        return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) {
        fprintf(stderr, "aura: AURA_CONTROL path is too long\n");
        return;
    }
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    unlink(path);
    mode_t old = umask(077);
    int r = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "aura: cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    listener = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(listener, ^{
        for (;;) {
            int c = accept(fd, NULL, NULL);
            if (c < 0)
                break;
            fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
            serve(c);
        }
    });
    dispatch_resume(listener);
    fprintf(stderr, "aura: control socket %s\n", path);
}
