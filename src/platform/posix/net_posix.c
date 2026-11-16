/* The AuNetOps over POSIX sockets (see posix.h): non-blocking BSD
 * sockets, getaddrinfo on a helper thread per lookup, the monotonic and
 * wall clocks, and the system's entropy. A port's main loop drives it
 * through au_posix_net_watch and au_posix_net_ready; a tool without one
 * calls au_posix_net_wait. */
#include "posix.h"

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
#include <time.h>
#include <unistd.h>

#define MAX_SOCKS (AU_POSIX_NET_MAX_WATCH - 1)
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

typedef struct Sock {
    int fd;          /* or -1: the slot is free */
    bool connecting; /* connect_poll has not yet seen the result */
    bool want_write; /* the last send took less than it was given */
} Sock;

struct AuPosixNet {
    AuNetOps ops;
    pthread_mutex_t mu;
    Lookup *lookups[MAX_LOOKUPS]; /* handle = index + 1 */
    Sock socks[MAX_SOCKS];        /* handle = index + 1 */
    int wake[2];                  /* lookup threads write a byte here */
    struct timespec start;
    int running;     /* lookup threads not yet finished (under mu) */
    unsigned serial; /* au_posix_net_watch_serial */
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
    AuPosixNet *h = ctx;
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
    AuPosixNet *h = ctx;
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
    AuPosixNet *h = ctx;
    if (req == 0 || req > MAX_LOOKUPS || !h->lookups[req - 1])
        return;
    Lookup *l = h->lookups[req - 1];
    h->lookups[req - 1] = NULL;
    lookup_release(l); /* the thread may still be running; it frees last */
}

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint32_t h_connect(void *ctx, const AuNetAddr *addr, uint16_t port)
{
    AuPosixNet *h = ctx;
    int slot = -1;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (h->socks[i].fd < 0) {
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
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    set_nonblocking(fd);
    if (connect(fd, (struct sockaddr *)&ss, sl) != 0 && errno != EINPROGRESS) {
        close(fd);
        return 0;
    }
    h->socks[slot].fd = fd;
    h->socks[slot].connecting = true;
    h->socks[slot].want_write = false;
    h->serial++;
    return (uint32_t)slot + 1;
}

static Sock *sock_of(AuPosixNet *h, uint32_t s)
{
    if (s == 0 || s > MAX_SOCKS || h->socks[s - 1].fd < 0)
        return NULL;
    return &h->socks[s - 1];
}

static void set_connected(AuPosixNet *h, Sock *k)
{
    if (k->connecting) {
        k->connecting = false;
        h->serial++;
    }
}

static int h_connect_poll(void *ctx, uint32_t s)
{
    AuPosixNet *h = ctx;
    Sock *k = sock_of(h, s);
    if (!k)
        return -1;
    struct pollfd p = {k->fd, POLLOUT, 0};
    int n = poll(&p, 1, 0);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    if (n == 0)
        return 0;
    set_connected(h, k);
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err)
        return -1;
    return 1;
}

static void set_want_write(AuPosixNet *h, Sock *k, bool on)
{
    if (k->want_write != on) {
        k->want_write = on;
        h->serial++;
    }
}

static long h_send(void *ctx, uint32_t s, const void *data, size_t len)
{
    AuPosixNet *h = ctx;
    Sock *k = sock_of(h, s);
    if (!k)
        return AU_NETIO_ERROR;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n = send(k->fd, data, len, flags);
    if (n >= 0) {
        set_want_write(h, k, (size_t)n < len);
        return (long)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        set_want_write(h, k, true);
        return 0;
    }
    return AU_NETIO_ERROR;
}

static long h_recv(void *ctx, uint32_t s, void *buf, size_t cap)
{
    Sock *k = sock_of(ctx, s);
    if (!k)
        return AU_NETIO_ERROR;
    ssize_t n = recv(k->fd, buf, cap, 0);
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
    AuPosixNet *h = ctx;
    Sock *k = sock_of(h, s);
    if (!k)
        return;
    close(k->fd);
    k->fd = -1;
    k->connecting = k->want_write = false;
    h->serial++;
}

static uint32_t h_now_ms(void *ctx)
{
    AuPosixNet *h = ctx;
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
    return au_posix_wall_time();
}

static bool h_entropy(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    return au_posix_entropy(buf, len);
}

AuPosixNet *au_posix_net_new(void)
{
    AuPosixNet *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    pthread_mutex_init(&h->mu, NULL);
    for (int i = 0; i < MAX_SOCKS; i++)
        h->socks[i].fd = -1;
    if (pipe(h->wake) != 0) {
        pthread_mutex_destroy(&h->mu);
        free(h);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        set_nonblocking(h->wake[i]);
        fcntl(h->wake[i], F_SETFD, FD_CLOEXEC);
    }
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

void au_posix_net_free(AuPosixNet *h)
{
    if (!h)
        return;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (h->socks[i].fd >= 0)
            close(h->socks[i].fd);
    /* Threads still running use the mutex: when a lookup is pending, the
     * AuPosixNet is left allocated rather than freeing the mutex under it. */
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

const AuNetOps *au_posix_net_ops(AuPosixNet *h)
{
    return &h->ops;
}

int au_posix_net_watch(AuPosixNet *h, AuPosixWatch *out, int cap)
{
    int n = 0;
    if (n < cap) {
        out[n].fd = h->wake[0];
        out[n].events = AU_POSIX_WATCH_READ;
        n++;
    }
    for (int i = 0; i < MAX_SOCKS && n < cap; i++) {
        const Sock *k = &h->socks[i];
        if (k->fd < 0)
            continue;
        out[n].fd = k->fd;
        out[n].events = AU_POSIX_WATCH_READ;
        if (k->connecting || k->want_write)
            out[n].events |= AU_POSIX_WATCH_WRITE;
        n++;
    }
    return n;
}

unsigned au_posix_net_watch_serial(AuPosixNet *h)
{
    return h->serial;
}

void au_posix_net_ready(AuPosixNet *h)
{
    char buf[64];
    while (read(h->wake[0], buf, sizeof buf) > 0) {
    }
}

void au_posix_net_wait(AuPosixNet *h, int ms)
{
    AuPosixWatch w[AU_POSIX_NET_MAX_WATCH];
    struct pollfd p[AU_POSIX_NET_MAX_WATCH];
    int n = au_posix_net_watch(h, w, AU_POSIX_NET_MAX_WATCH);
    for (int i = 0; i < n; i++) {
        p[i].fd = w[i].fd;
        p[i].events = (short)(((w[i].events & AU_POSIX_WATCH_READ) ? POLLIN : 0) |
                              ((w[i].events & AU_POSIX_WATCH_WRITE) ? POLLOUT : 0));
        p[i].revents = 0;
    }
    if (poll(p, (nfds_t)n, ms) > 0)
        au_posix_net_ready(h);
}
