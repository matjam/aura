/* AuNetOps with Open Transport. See net_ot.h.
 *
 * Slots. Sockets and lookups live in fixed arrays inside the AuOtNet,
 * which is held in memory (HoldMemory) because notifiers write to it at
 * deferred task time. A handle is (generation << 8) | (index + 1), so a
 * stale handle never reaches a reused slot.
 *
 * Notifier fields. Each slot has bytes and results that only its notifier
 * writes (ev_*, *_result, open_ref) and that the main task clears before
 * acting on them. A completion arriving while the main task works is then
 * seen on the next call, never lost.
 *
 * Socket states: OPENING (OTAsyncOpenEndpoint sent) -> BINDING (OTBind
 * sent) -> CONNECTING (OTConnect sent) -> CONNECTED, or FAILED. A socket
 * closed while OPENING becomes a ZOMBIE: the slot stays until the open
 * completes, and au_ot_net_idle then closes the endpoint. Lookups go
 * OPENING (OTAsyncOpenInternetServices) -> QUERYING
 * (OTInetStringToAddress) -> DONE or FAILED, with the same ZOMBIE rule.
 * A lookup's provider is closed as soon as its answer arrives. */
#include "net_ot.h"

#include "clock_mac.h"
#include "core/core.h"
#include "ot.h"

#include <string.h>

enum { kSocks = 24, kLookups = 16, kSendMax = 32768 };

typedef enum { S_FREE, S_OPENING, S_BINDING, S_CONNECTING, S_CONNECTED, S_FAILED, S_ZOMBIE } SockState;
typedef enum { L_FREE, L_OPENING, L_QUERYING, L_DONE, L_FAILED, L_ZOMBIE } LookState;

typedef struct OtSock {
    /* written by the notifier */
    volatile uint8_t ev_open, ev_bind, ev_connect, ev_disc;
    volatile AuOtResult open_result, bind_result, connect_result;
    void *volatile open_ref;
    /* main task only */
    uint8_t state, gen;
    bool eof;
    AuOtRef ep;
    AuOtInetAddress dest;
    AuOtCall call;
    struct AuOtNet *owner;
} OtSock;

typedef struct OtLookup {
    volatile uint8_t ev_open, ev_done;
    volatile AuOtResult open_result, done_result;
    void *volatile open_ref;
    uint8_t state, gen;
    AuOtRef svc;
    uint32_t addr; /* network byte order, as Open Transport gives it */
    char name[256];
    AuOtInetHostInfo info;
} OtLookup;

struct AuOtNet {
    AuOtClient client;
    UInt32 client_words[3];
    AuNetOps ops;
    AuMacEntropy *entropy;
    OtSock socks[kSocks];
    OtLookup lookups[kLookups];
    AuOtNetStats stats;
    bool held;
};

static void barrier(void)
{
#if defined(__powerpc__) || defined(__ppc__)
    __asm__ __volatile__("sync" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* ---- Notifiers (deferred task time: record only) ---- */

static pascal void sock_notify(void *ctx, AuOtEvent code, AuOtResult result, void *cookie)
{
    OtSock *s = ctx;
    if (code == kAuOtEvOpenComplete) {
        s->open_ref = cookie;
        s->open_result = result;
        barrier();
        s->ev_open = 1;
    } else if (code == kAuOtEvBindComplete) {
        s->bind_result = result;
        barrier();
        s->ev_bind = 1;
    } else if (code == kAuOtEvConnect) {
        s->connect_result = result;
        barrier();
        s->ev_connect = 1;
    } else if (code == kAuOtEvDisconnect) {
        s->ev_disc = 1;
    }
}

static pascal void lookup_notify(void *ctx, AuOtEvent code, AuOtResult result, void *cookie)
{
    OtLookup *l = ctx;
    if (code == kAuOtEvOpenComplete) {
        l->open_ref = cookie;
        l->open_result = result;
        barrier();
        l->ev_open = 1;
    } else if (code == kAuOtEvDnrStringToAddrComplete) {
        l->done_result = result;
        barrier();
        l->ev_done = 1;
    }
}

/* ---- Handles ---- */

static uint32_t make_handle(uint8_t gen, int idx)
{
    return ((uint32_t)gen << 8) | (uint32_t)(idx + 1);
}

static uint8_t next_gen(uint8_t g)
{
    g++;
    return g ? g : 1;
}

static OtSock *sock_of(AuOtNet *n, uint32_t h)
{
    uint32_t idx = (h & 0xFF);
    if (idx < 1 || idx > kSocks || (h >> 16))
        return NULL;
    OtSock *s = &n->socks[idx - 1];
    if (s->state == S_FREE || s->state == S_ZOMBIE || s->gen != (uint8_t)(h >> 8))
        return NULL;
    return s;
}

static OtLookup *lookup_of(AuOtNet *n, uint32_t h)
{
    uint32_t idx = (h & 0xFF);
    if (idx < 1 || idx > kLookups || (h >> 16))
        return NULL;
    OtLookup *l = &n->lookups[idx - 1];
    if (l->state == L_FREE || l->state == L_ZOMBIE || l->gen != (uint8_t)(h >> 8))
        return NULL;
    return l;
}

static void note_error(AuOtNet *n, long err)
{
    if (err)
        n->stats.last_error = err;
}

static void close_provider(AuOtNet *n, AuOtRef ref)
{
    if (ref)
        OTCloseProviderPriv(ref, &n->client);
}

static void time_sample(AuOtNet *n)
{
    /* Network timings are visible to others on the path: mixed, not
     * credited. */
    uint32_t t = au_mac_clock_ms();
    au_mac_entropy_add(n->entropy, &t, sizeof t, 0);
}

/* ---- DNS ---- */

static bool parse_ipv4(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return false;
        }
        out[i] = (uint8_t)v;
        if (i < 3) {
            if (*s != '.')
                return false;
            s++;
        }
    }
    return *s == 0;
}

static void lookup_step(AuOtNet *n, OtLookup *l)
{
    if (l->state == L_OPENING && l->ev_open) {
        l->ev_open = 0;
        barrier();
        if (l->open_result != 0) {
            note_error(n, l->open_result);
            l->state = L_FAILED;
            return;
        }
        l->svc = l->open_ref;
        OSStatus e = OTInetStringToAddress(l->svc, l->name, &l->info);
        if (e != 0) {
            note_error(n, e);
            close_provider(n, l->svc);
            l->svc = NULL;
            l->state = L_FAILED;
            return;
        }
        l->state = L_QUERYING;
    }
    if (l->state == L_QUERYING && l->ev_done) {
        l->ev_done = 0;
        barrier();
        uint32_t a = l->info.addrs[0];
        close_provider(n, l->svc);
        l->svc = NULL;
        time_sample(n);
        if (l->done_result != 0 || a == 0) {
            note_error(n, l->done_result);
            l->state = L_FAILED;
        } else {
            l->addr = a;
            l->state = L_DONE;
        }
    }
}

static uint32_t op_resolve_start(void *ctx, const char *host)
{
    AuOtNet *n = ctx;
    size_t len = strlen(host);
    if (len == 0 || len > 255 || strchr(host, ':'))
        return 0;
    int idx = -1;
    for (int i = 0; i < kLookups; i++)
        if (n->lookups[i].state == L_FREE) {
            idx = i;
            break;
        }
    if (idx < 0)
        return 0;
    OtLookup *l = &n->lookups[idx];
    uint8_t gen = next_gen(l->gen);
    memset(l, 0, sizeof *l);
    l->gen = gen;
    memcpy(l->name, host, len + 1);
    n->stats.lookups++;
    uint8_t lit[4];
    if (parse_ipv4(host, lit)) {
        memcpy(&l->addr, lit, 4);
        l->state = L_DONE;
        return make_handle(gen, idx);
    }
    l->state = L_OPENING;
    OSStatus e = OTAsyncOpenInternetServicesPriv(kAuOtDefaultInternetServices, 0, lookup_notify, l, &n->client);
    if (e != 0) {
        note_error(n, e);
        l->state = L_FREE;
        n->stats.lookups_failed++;
        return 0;
    }
    return make_handle(gen, idx);
}

static int op_resolve_poll(void *ctx, uint32_t req, AuNetAddr *addr)
{
    AuOtNet *n = ctx;
    OtLookup *l = lookup_of(n, req);
    if (!l)
        return -1;
    lookup_step(n, l);
    if (l->state == L_DONE) {
        memset(addr, 0, sizeof *addr);
        addr->family = 4;
        memcpy(addr->addr, &l->addr, 4);
        return 1;
    }
    if (l->state == L_FAILED)
        return -1;
    return 0;
}

static void op_resolve_cancel(void *ctx, uint32_t req)
{
    AuOtNet *n = ctx;
    OtLookup *l = lookup_of(n, req);
    if (!l)
        return;
    if (l->state == L_FAILED)
        n->stats.lookups_failed++;
    if (l->state == L_OPENING) {
        l->state = L_ZOMBIE; /* closed by au_ot_net_idle when the open completes */
        return;
    }
    if (l->state == L_QUERYING) {
        close_provider(n, l->svc); /* cancels the query */
        l->svc = NULL;
    }
    l->state = L_FREE;
}

/* ---- TCP ---- */

static void sock_fail(AuOtNet *n, OtSock *s, long err)
{
    note_error(n, err ? err : -1);
    s->state = S_FAILED;
}

static void sock_step(AuOtNet *n, OtSock *s)
{
    if (s->state == S_OPENING && s->ev_open) {
        s->ev_open = 0;
        barrier();
        if (s->open_result != 0) {
            sock_fail(n, s, s->open_result);
            return;
        }
        s->ep = s->open_ref;
        OTSetNonBlocking(s->ep);
        OSStatus e = OTBind(s->ep, NULL, NULL);
        if (e != 0) {
            sock_fail(n, s, e);
            return;
        }
        s->state = S_BINDING;
    }
    if (s->state == S_BINDING && s->ev_bind) {
        s->ev_bind = 0;
        barrier();
        if (s->bind_result != 0) {
            sock_fail(n, s, s->bind_result);
            return;
        }
        memset(&s->call, 0, sizeof s->call);
        s->call.addr.buf = (UInt8 *)&s->dest;
        s->call.addr.len = sizeof s->dest;
        s->call.addr.maxlen = sizeof s->dest;
        OSStatus e = OTConnect(s->ep, &s->call, NULL);
        if (e != 0 && e != kAuOtNoDataErr) {
            sock_fail(n, s, e);
            return;
        }
        s->state = S_CONNECTING;
    }
    if (s->state == S_CONNECTING) {
        if (s->ev_connect) {
            s->ev_connect = 0;
            barrier();
            OSStatus e = OTRcvConnect(s->ep, NULL);
            time_sample(n);
            if (e != 0) {
                sock_fail(n, s, e);
                return;
            }
            s->state = S_CONNECTED;
        } else if (s->ev_disc) {
            s->ev_disc = 0;
            OTRcvDisconnect(s->ep, NULL);
            sock_fail(n, s, -3260 /* kOTECONNREFUSEDErr, or another refusal */);
        }
    }
}

static uint32_t op_connect(void *ctx, const AuNetAddr *addr, uint16_t port)
{
    AuOtNet *n = ctx;
    if (addr->family != 4)
        return 0;
    int idx = -1;
    for (int i = 0; i < kSocks; i++)
        if (n->socks[i].state == S_FREE) {
            idx = i;
            break;
        }
    if (idx < 0)
        return 0;
    OtSock *s = &n->socks[idx];
    uint8_t gen = next_gen(s->gen);
    memset(s, 0, sizeof *s);
    s->gen = gen;
    s->owner = n;
    s->dest.fAddressType = kAuOtAfInet;
    s->dest.fPort = port;
    memcpy(&s->dest.fHost, addr->addr, 4);
    n->stats.connects++;
    AuOtConfig *cfg = OTCreateConfiguration("tcp");
    if (!cfg) {
        n->stats.connects_failed++;
        return 0;
    }
    s->state = S_OPENING;
    OSStatus e = OTAsyncOpenEndpointPriv(cfg, 0, NULL, sock_notify, s, &n->client);
    if (e != 0) {
        note_error(n, e);
        s->state = S_FREE;
        n->stats.connects_failed++;
        return 0;
    }
    return make_handle(gen, idx);
}

static int op_connect_poll(void *ctx, uint32_t h)
{
    AuOtNet *n = ctx;
    OtSock *s = sock_of(n, h);
    if (!s)
        return -1;
    sock_step(n, s);
    if (s->state == S_CONNECTED)
        return 1;
    if (s->state == S_FAILED)
        return -1;
    return 0;
}

/* After kAuOtLookErr: takes the pending event. Returns AU_NETIO_CLOSED,
 * AU_NETIO_ERROR, or AU_NETIO_WOULD_BLOCK to try again later. */
static long take_look(AuOtNet *n, OtSock *s)
{
    AuOtResult ev = OTLook(s->ep);
    if (ev == kAuOtEvOrdRel) {
        OTRcvOrderlyDisconnect(s->ep);
        s->eof = true;
        return AU_NETIO_CLOSED;
    }
    if (ev == kAuOtEvDisconnect) {
        OTRcvDisconnect(s->ep, NULL);
        sock_fail(n, s, -3259 /* reset */);
        return AU_NETIO_ERROR;
    }
    if (ev == kAuOtEvData || ev == kAuOtEvGoData || ev == 0)
        return AU_NETIO_WOULD_BLOCK;
    sock_fail(n, s, ev < 0 ? ev : -1);
    return AU_NETIO_ERROR;
}

static long op_send(void *ctx, uint32_t h, const void *data, size_t len)
{
    AuOtNet *n = ctx;
    OtSock *s = sock_of(n, h);
    if (!s || s->state != S_CONNECTED)
        return AU_NETIO_ERROR;
    if (len > kSendMax)
        len = kSendMax;
    AuOtResult r = OTSnd(s->ep, (void *)data, (UInt32)len, 0);
    if (r >= 0) {
        n->stats.bytes_sent += (unsigned long)r;
        return r;
    }
    if (r == kAuOtFlowErr)
        return 0;
    if (r == kAuOtLookErr) {
        long t = take_look(n, s);
        /* The peer's orderly release closes its side only; sending may go
         * on, so try again later. */
        return t == AU_NETIO_ERROR ? AU_NETIO_ERROR : 0;
    }
    sock_fail(n, s, r);
    return AU_NETIO_ERROR;
}

static long op_recv(void *ctx, uint32_t h, void *buf, size_t cap)
{
    AuOtNet *n = ctx;
    OtSock *s = sock_of(n, h);
    if (!s || s->state != S_CONNECTED)
        return AU_NETIO_ERROR;
    if (s->eof)
        return AU_NETIO_CLOSED;
    if (cap == 0)
        return AU_NETIO_WOULD_BLOCK;
    if (cap > 0x7FFFFFFF)
        cap = 0x7FFFFFFF;
    UInt32 flags = 0;
    AuOtResult r = OTRcv(s->ep, buf, (UInt32)cap, &flags);
    if (r > 0) {
        n->stats.bytes_received += (unsigned long)r;
        return r;
    }
    if (r == 0 || r == kAuOtNoDataErr)
        return AU_NETIO_WOULD_BLOCK;
    if (r == kAuOtLookErr)
        return take_look(n, s);
    sock_fail(n, s, r);
    return AU_NETIO_ERROR;
}

static void op_close(void *ctx, uint32_t h)
{
    AuOtNet *n = ctx;
    OtSock *s = sock_of(n, h);
    if (!s)
        return;
    if (s->state == S_FAILED)
        n->stats.connects_failed++;
    if (s->state == S_OPENING) {
        s->state = S_ZOMBIE;
        return;
    }
    /* An abortive close; no notifier call follows its return. */
    close_provider(n, s->ep);
    s->ep = NULL;
    s->state = S_FREE;
}

/* ---- Clocks and entropy ---- */

static uint32_t op_now_ms(void *ctx)
{
    (void)ctx;
    return au_mac_clock_ms();
}

static int64_t op_wall_time(void *ctx)
{
    (void)ctx;
    return au_mac_wall_time();
}

static bool op_entropy(void *ctx, void *buf, size_t len)
{
    AuOtNet *n = ctx;
    return au_mac_entropy_read(n->entropy, buf, len);
}

/* ---- Public ---- */

AuOtNet *au_ot_net_new(AuMacEntropy *entropy, long *err)
{
    if (err)
        *err = 0;
    long resp = 0;
    if (Gestalt('otan', &resp) != noErr || !(resp & kAuOtPresent) || !(resp & kAuOtTcpPresent)) {
        if (err)
            *err = 1;
        return NULL;
    }
    AuOtNet *n = au_alloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->held = HoldMemory(n, sizeof *n) == noErr;
    n->entropy = entropy;
    n->client.word8 = &n->client_words[0];
    n->client.word12 = &n->client_words[1];
    n->client.word16 = &n->client_words[2];
    n->client.flag = 1;
    OSStatus e = InitOpenTransportPriv(kAuOtInitApplication, &n->client);
    if (e != noErr) {
        if (err)
            *err = e;
        if (n->held)
            UnholdMemory(n, sizeof *n);
        au_free(n);
        return NULL;
    }
    n->ops.ctx = n;
    n->ops.resolve_start = op_resolve_start;
    n->ops.resolve_poll = op_resolve_poll;
    n->ops.resolve_cancel = op_resolve_cancel;
    n->ops.connect = op_connect;
    n->ops.connect_poll = op_connect_poll;
    n->ops.send = op_send;
    n->ops.recv = op_recv;
    n->ops.close = op_close;
    n->ops.now_ms = op_now_ms;
    n->ops.wall_time = op_wall_time;
    n->ops.entropy = op_entropy;
    return n;
}

const AuNetOps *au_ot_net_ops(AuOtNet *n)
{
    return &n->ops;
}

void au_ot_net_idle(AuOtNet *n)
{
    for (int i = 0; i < kSocks; i++) {
        OtSock *s = &n->socks[i];
        if (s->state == S_ZOMBIE && s->ev_open) {
            s->ev_open = 0;
            barrier();
            if (s->open_result == 0)
                close_provider(n, s->open_ref);
            s->state = S_FREE;
        }
    }
    for (int i = 0; i < kLookups; i++) {
        OtLookup *l = &n->lookups[i];
        if (l->state == L_ZOMBIE && l->ev_open) {
            l->ev_open = 0;
            barrier();
            if (l->open_result == 0)
                close_provider(n, l->open_ref);
            l->state = L_FREE;
        }
    }
}

bool au_ot_net_busy(const AuOtNet *n)
{
    for (int i = 0; i < kSocks; i++)
        if (n->socks[i].state != S_FREE)
            return true;
    for (int i = 0; i < kLookups; i++)
        if (n->lookups[i].state != L_FREE)
            return true;
    return false;
}

void au_ot_net_stats(const AuOtNet *n, AuOtNetStats *out)
{
    *out = n->stats;
}

void au_ot_net_free(AuOtNet *n)
{
    if (!n)
        return;
    for (int i = 0; i < kSocks; i++) {
        OtSock *s = &n->socks[i];
        if (s->ep)
            close_provider(n, s->ep);
        else if (s->state == S_OPENING || s->state == S_ZOMBIE) {
            if (s->ev_open && s->open_result == 0)
                close_provider(n, s->open_ref);
        }
    }
    for (int i = 0; i < kLookups; i++) {
        OtLookup *l = &n->lookups[i];
        if (l->svc)
            close_provider(n, l->svc);
        else if ((l->state == L_OPENING || l->state == L_ZOMBIE) && l->ev_open && l->open_result == 0)
            close_provider(n, l->open_ref);
    }
    /* Closing Open Transport also closes any provider whose open was
     * still in flight, so no notifier runs after this. */
    CloseOpenTransportPriv(&n->client);
    if (n->held)
        UnholdMemory(n, sizeof *n);
    au_free(n);
}
