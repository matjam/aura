/* The host's AuNetOps (see net_host.h). */
#include "net_host.h"
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/roots.h"
#include "net/web_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/random.h>
#endif

#define MAX_SOCKS 256
#define MAX_LOOKUPS 64

/* A lookup shared between the owner and its thread; whichever finishes
 * last frees it. */
typedef struct Lookup {
    pthread_mutex_t *mu;
    char host[256];
    int state; /* 0 pending, 1 done, -1 failed */
    AuNetAddr addr;
    int refs;
} Lookup;

struct AuHostNet {
    AuNetOps ops;
    pthread_mutex_t mu;
    Lookup *lookups[MAX_LOOKUPS]; /* handle = index + 1 */
    int socks[MAX_SOCKS];         /* fd, or -1; handle = index + 1 */
    int wake[2];                  /* lookup threads write a byte here */
    struct timespec start;
    int running; /* lookup threads not yet finished (under mu) */
};

static void lookup_release(Lookup *l)
{
    pthread_mutex_lock(l->mu);
    int left = --l->refs;
    pthread_mutex_unlock(l->mu);
    if (!left)
        free(l);
}

typedef struct ThreadArg {
    Lookup *l;
    int wake_fd;
    int *running;
} ThreadArg;

static void *lookup_thread(void *p)
{
    ThreadArg a = *(ThreadArg *)p;
    free(p);
    Lookup *l = a.l;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int rc = getaddrinfo(l->host, NULL, &hints, &res);
    AuNetAddr addr;
    memset(&addr, 0, sizeof addr);
    bool ok = false;
    /* Prefer IPv4: it is what the Mac OS 9 build has, so host results
     * match the guest's. */
    for (int pass = 0; pass < 2 && !ok && rc == 0; pass++) {
        for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
            if (pass == 0 && ai->ai_family == AF_INET) {
                addr.family = 4;
                memcpy(addr.addr, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, 4);
                ok = true;
            } else if (pass == 1 && ai->ai_family == AF_INET6) {
                addr.family = 6;
                memcpy(addr.addr, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, 16);
                ok = true;
            }
        }
    }
    if (res)
        freeaddrinfo(res);
    pthread_mutex_lock(l->mu);
    l->addr = addr;
    l->state = ok ? 1 : -1;
    pthread_mutex_unlock(l->mu);
    char b = 1;
    ssize_t w = write(a.wake_fd, &b, 1);
    (void)w;
    pthread_mutex_t *mu = l->mu;
    lookup_release(l);
    pthread_mutex_lock(mu);
    (*a.running)--;
    pthread_mutex_unlock(mu);
    return NULL;
}

static uint32_t h_resolve_start(void *ctx, const char *host)
{
    AuHostNet *h = ctx;
    if (strlen(host) >= sizeof ((Lookup *)0)->host)
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (!h->lookups[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return 0;
    Lookup *l = calloc(1, sizeof *l);
    if (!l)
        return 0;
    l->mu = &h->mu;
    snprintf(l->host, sizeof l->host, "%s", host);
    l->refs = 2;
    ThreadArg *a = malloc(sizeof *a);
    if (!a) {
        free(l);
        return 0;
    }
    a->l = l;
    a->wake_fd = h->wake[1];
    a->running = &h->running;
    pthread_mutex_lock(&h->mu);
    h->running++;
    pthread_mutex_unlock(&h->mu);
    pthread_t th;
    if (pthread_create(&th, NULL, lookup_thread, a) != 0) {
        pthread_mutex_lock(&h->mu);
        h->running--;
        pthread_mutex_unlock(&h->mu);
        free(a);
        free(l);
        return 0;
    }
    pthread_detach(th);
    h->lookups[slot] = l;
    return (uint32_t)slot + 1;
}

static int h_resolve_poll(void *ctx, uint32_t req, AuNetAddr *addr)
{
    AuHostNet *h = ctx;
    if (req == 0 || req > MAX_LOOKUPS || !h->lookups[req - 1])
        return -1;
    Lookup *l = h->lookups[req - 1];
    pthread_mutex_lock(&h->mu);
    int st = l->state;
    if (st == 1)
        *addr = l->addr;
    pthread_mutex_unlock(&h->mu);
    return st;
}

static void h_resolve_cancel(void *ctx, uint32_t req)
{
    AuHostNet *h = ctx;
    if (req == 0 || req > MAX_LOOKUPS || !h->lookups[req - 1])
        return;
    Lookup *l = h->lookups[req - 1];
    h->lookups[req - 1] = NULL;
    lookup_release(l); /* the thread may still be running; it frees last */
}

static uint32_t h_connect(void *ctx, const AuNetAddr *addr, uint16_t port)
{
    AuHostNet *h = ctx;
    int slot = -1;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (h->socks[i] < 0) {
            slot = i;
            break;
        }
    if (slot < 0)
        return 0;
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    if (addr->family == 4) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        memcpy(&s4->sin_addr, addr->addr, 4);
        sl = sizeof *s4;
    } else if (addr->family == 6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        memcpy(&s6->sin6_addr, addr->addr, 16);
        sl = sizeof *s6;
    } else {
        return 0;
    }
    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&ss, sl) != 0 && errno != EINPROGRESS) {
        close(fd);
        return 0;
    }
    h->socks[slot] = fd;
    return (uint32_t)slot + 1;
}

static int sock_fd(AuHostNet *h, uint32_t s)
{
    if (s == 0 || s > MAX_SOCKS)
        return -1;
    return h->socks[s - 1];
}

static int h_connect_poll(void *ctx, uint32_t s)
{
    int fd = sock_fd(ctx, s);
    if (fd < 0)
        return -1;
    struct pollfd p = {fd, POLLOUT, 0};
    int n = poll(&p, 1, 0);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    if (n == 0)
        return 0;
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err)
        return -1;
    return 1;
}

static long h_send(void *ctx, uint32_t s, const void *data, size_t len)
{
    int fd = sock_fd(ctx, s);
    if (fd < 0)
        return AU_NETIO_ERROR;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n = send(fd, data, len, flags);
    if (n >= 0)
        return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    return AU_NETIO_ERROR;
}

static long h_recv(void *ctx, uint32_t s, void *buf, size_t cap)
{
    int fd = sock_fd(ctx, s);
    if (fd < 0)
        return AU_NETIO_ERROR;
    ssize_t n = recv(fd, buf, cap, 0);
    if (n > 0)
        return (long)n;
    if (n == 0)
        return AU_NETIO_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return AU_NETIO_WOULD_BLOCK;
    return AU_NETIO_ERROR;
}

static void h_close(void *ctx, uint32_t s)
{
    AuHostNet *h = ctx;
    int fd = sock_fd(h, s);
    if (fd < 0)
        return;
    close(fd);
    h->socks[s - 1] = -1;
}

static uint32_t h_now_ms(void *ctx)
{
    AuHostNet *h = ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ms = (int64_t)(ts.tv_sec - h->start.tv_sec) * 1000 + (ts.tv_nsec - h->start.tv_nsec) / 1000000;
    return (uint32_t)ms;
}

static int64_t h_wall_time(void *ctx)
{
    (void)ctx;
    const char *fake = getenv("AURA_FAKE_TIME"); /* for testing the clock check */
    if (fake && *fake)
        return (int64_t)strtoll(fake, NULL, 10);
    return (int64_t)time(NULL);
}

static bool h_entropy(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    uint8_t *p = buf;
    while (len) {
        size_t n = len > 256 ? 256 : len;
        if (getentropy(p, n) != 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

AuHostNet *au_host_net_new(void)
{
    AuHostNet *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    pthread_mutex_init(&h->mu, NULL);
    for (int i = 0; i < MAX_SOCKS; i++)
        h->socks[i] = -1;
    if (pipe(h->wake) != 0) {
        free(h);
        return NULL;
    }
    fcntl(h->wake[0], F_SETFL, fcntl(h->wake[0], F_GETFL) | O_NONBLOCK);
    fcntl(h->wake[1], F_SETFL, fcntl(h->wake[1], F_GETFL) | O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &h->start);
    h->ops.ctx = h;
    h->ops.resolve_start = h_resolve_start;
    h->ops.resolve_poll = h_resolve_poll;
    h->ops.resolve_cancel = h_resolve_cancel;
    h->ops.connect = h_connect;
    h->ops.connect_poll = h_connect_poll;
    h->ops.send = h_send;
    h->ops.recv = h_recv;
    h->ops.close = h_close;
    h->ops.now_ms = h_now_ms;
    h->ops.wall_time = h_wall_time;
    h->ops.entropy = h_entropy;
    return h;
}

void au_host_net_free(AuHostNet *h)
{
    if (!h)
        return;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (h->socks[i] >= 0)
            close(h->socks[i]);
    /* Threads still running use the mutex: when a lookup is pending, the
     * AuHostNet is left allocated rather than freeing the mutex under it. */
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (h->lookups[i])
            lookup_release(h->lookups[i]);
    pthread_mutex_lock(&h->mu);
    bool pending = h->running > 0;
    pthread_mutex_unlock(&h->mu);
    if (pending)
        return;
    close(h->wake[0]);
    close(h->wake[1]);
    pthread_mutex_destroy(&h->mu);
    free(h);
}

const AuNetOps *au_host_net_ops(AuHostNet *h)
{
    return &h->ops;
}

void au_host_net_wait(AuHostNet *h, int ms)
{
    struct pollfd p[MAX_SOCKS + 1];
    int n = 0;
    p[n].fd = h->wake[0];
    p[n].events = POLLIN;
    p[n].revents = 0;
    n++;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (h->socks[i] >= 0) {
            p[n].fd = h->socks[i];
            p[n].events = POLLIN; /* writability is checked by the pump itself */
            p[n].revents = 0;
            n++;
        }
    int r = poll(p, (nfds_t)n, ms);
    if (r > 0 && (p[0].revents & POLLIN)) {
        char buf[64];
        while (read(h->wake[0], buf, sizeof buf) > 0) {
        }
    }
}

char *au_host_read_file(const char *path, size_t *len)
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

bool au_host_write_file(const char *path, const void *data, size_t len)
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

bool au_host_profile_path(const char *name, char *out, size_t cap)
{
    const char *dir = getenv("AURA_PROFILE");
    char buf[2048];
    if (!dir || !*dir) {
        const char *home = getenv("HOME");
        if (!home)
            return false;
        snprintf(buf, sizeof buf, "%s/.aura-host", home);
        dir = buf;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    return snprintf(out, cap, "%s/%s", dir, name) < (int)cap;
}

/* ---- The profile folder, through browser/app.c ----
 * Web storage's files in the folder "Local Storage", and the Cookies,
 * HSTS and Intermediates files, loaded and saved by the application
 * services as on Mac OS 9 (a file that does not parse is renamed
 * "<name> (damaged)"). */

static AuAppFiles profile_files;
static AuAppFolder ls_folder;

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
    s->host = au_host_net_new();
    if (!s->host)
        return false;
    const AuNetOps *ops = au_host_net_ops(s->host);
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
    const AuNetOps *ops = au_host_net_ops(s->host);
    au_app_save_stores(files(), s->jar, s->hsts, s->net, ops->wall_time(ops->ctx));
    au_net_free(s->net);
    if (s->jar)
        au_cookie_jar_free(s->jar);
    au_hsts_free(s->hsts);
    if (s->roots)
        au_roots_free(s->roots);
    au_host_net_free(s->host);
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
    char *data = au_host_read_file(path, &len);
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
