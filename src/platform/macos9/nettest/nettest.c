/* NetTest: Aura's network stack on Mac OS 9, in a console window.
 *
 * It starts the entropy pool and Open Transport (src/platform/macos9),
 * times the crypto primitives TLS uses, resolves and connects to one host
 * through AuNetOps directly, then fetches a list of URLs with the engine
 * (src/net) and prints, for each, the status, TLS version and cipher,
 * time to the response headers and to the end, and the body size.
 *
 * Files beside the application:
 *   aura-roots.bin   the root store (assets/net/aura-roots.bin), or the
 *   or aura-roots    same file without the extension; without it https
 *                    requests fail with a certificate error
 *   NetTest URLs     optional, one URL per line, replaces the built-in list
 * The output is also written to "NetTest Log" in the Aura folder of the
 * Preferences folder. When the list is done, a key or click quits.
 *
 * Everything runs on the main task: WaitNextEvent with a short sleep,
 * then au_net_pump, au_ot_net_idle and the entropy pool's idle step. */
#include <Multiverse.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "core/core.h"
#include "net/net.h"
#include "net/roots.h"

#include "../clock_mac.h"
#include "../entropy.h"
#include "../net_ot.h"

enum { kMaxUrls = 16 };

static const char *const kDefaultUrls[] = {
    "http://example.com/",
    "https://example.com/",
    "https://en.wikipedia.org/wiki/Mac_OS_9",
    "https://news.ycombinator.com/",
    "https://html.duckduckgo.com/html/?q=mac+os+9",
    "https://old.reddit.com/",
};

typedef struct Log {
    char *buf;
    size_t len, cap;
} Log;

typedef struct Run {
    Log log;
    AuMacEntropy *entropy;
    AuOtNet *ot;
    AuNet *net;
} Run;

static void say(Run *r, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    fputs(line, stdout);
    fflush(stdout);
    if (r->log.len + (size_t)n + 1 > r->log.cap) {
        size_t cap = r->log.cap ? r->log.cap * 2 : 4096;
        while (cap < r->log.len + (size_t)n + 1)
            cap *= 2;
        r->log.buf = au_realloc(r->log.buf, cap);
        r->log.cap = cap;
    }
    memcpy(r->log.buf + r->log.len, line, (size_t)n);
    r->log.len += (size_t)n;
    r->log.buf[r->log.len] = 0;
}

/* One pass of the event loop: events, the network, the entropy pool. */
static void pass(Run *r, long sleep)
{
    EventRecord ev;
    if (WaitNextEvent(everyEvent, &ev, sleep, NULL))
        au_mac_entropy_event(r->entropy, &ev);
    au_mac_entropy_idle(r->entropy);
    if (r->net)
        au_net_pump(r->net);
    if (r->ot)
        au_ot_net_idle(r->ot);
}

/* ---- Files beside the application ---- */

static bool app_dir(short *vref, long *dir)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec spec;
    Str255 name;
    if (GetCurrentProcess(&psn) != noErr)
        return false;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processName = name;
    info.processAppSpec = &spec;
    if (GetProcessInformation(&psn, &info) != noErr)
        return false;
    *vref = spec.vRefNum;
    *dir = spec.parID;
    return true;
}

static char *read_beside(const char *fname, size_t *len)
{
    short vref;
    long dir;
    if (!app_dir(&vref, &dir))
        return NULL;
    Str255 p;
    size_t n = strlen(fname);
    p[0] = (unsigned char)n;
    memcpy(p + 1, fname, n);
    FSSpec spec;
    if (FSMakeFSSpec(vref, dir, p, &spec) != noErr)
        return NULL;
    short ref;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
        return NULL;
    long eof = 0;
    char *buf = NULL;
    if (GetEOF(ref, &eof) == noErr && eof >= 0 && eof < 4L * 1024 * 1024) {
        buf = au_alloc((size_t)eof + 1);
        long count = eof;
        OSErr err = FSRead(ref, &count, buf);
        if ((err == noErr || err == eofErr) && count == eof) {
            buf[eof] = 0;
            *len = (size_t)eof;
        } else {
            au_free(buf);
            buf = NULL;
        }
    }
    FSClose(ref);
    return buf;
}

static void write_log(Run *r)
{
    short vref;
    long dir;
    if (FindFolder(-32768, kPreferencesFolderType, true, &vref, &dir) != noErr)
        return;
    Str255 aura = {4, 'A', 'u', 'r', 'a'};
    FSSpec spec;
    CInfoPBRec pb;
    if (FSMakeFSSpec(vref, dir, aura, &spec) != noErr)
        return;
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = spec.name;
    pb.dirInfo.ioVRefNum = spec.vRefNum;
    pb.dirInfo.ioDrDirID = spec.parID;
    if (PBGetCatInfoSync(&pb) != noErr)
        return;
    long sub = pb.dirInfo.ioDrDirID;
    Str255 name = {11, 'N', 'e', 't', 'T', 'e', 's', 't', ' ', 'L', 'o', 'g'};
    OSErr err = FSMakeFSSpec(vref, sub, name, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'ttxt', 'TEXT', -1);
    if (err != noErr)
        return;
    short ref;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr)
        return;
    /* Mac line endings */
    for (size_t i = 0; i < r->log.len; i++)
        if (r->log.buf[i] == '\n')
            r->log.buf[i] = '\r';
    long count = (long)r->log.len;
    if (FSWrite(ref, &count, r->log.buf) == noErr)
        SetEOF(ref, count);
    FSClose(ref);
    FlushVol(NULL, vref);
}

/* ---- Crypto timings ---- */

static void bench(Run *r)
{
    static unsigned char buf[16384];
    memset(buf, 0x5A, sizeof buf);
    unsigned char key[32], iv[12], tag[16];
    memset(key, 1, sizeof key);
    memset(iv, 2, sizeof iv);

    uint32_t t0 = au_mac_clock_ms(), t;
    int n = 0;
    do {
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, buf, sizeof buf);
        br_sha256_out(&c, tag);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 500);
    say(r, "sha256         %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));

    br_aes_ct_ctr_keys aes;
    br_aes_ct_ctr_init(&aes, key, 16);
    unsigned char h[16] = {3}, y[16] = {0};
    t0 = au_mac_clock_ms();
    n = 0;
    do {
        br_aes_ct_ctr_run(&aes, iv, 1, buf, sizeof buf);
        br_ghash_ctmul32(y, h, buf, sizeof buf);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 500);
    say(r, "aes128-gcm     %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));

    t0 = au_mac_clock_ms();
    n = 0;
    do {
        br_poly1305_ctmul_run(key, iv, buf, sizeof buf, NULL, 0, tag, br_chacha20_ct_run, 1);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 500);
    say(r, "chacha20-poly  %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));

    unsigned char point[133], scalar[32];
    memset(scalar, 0x37, sizeof scalar);
    const br_ec_impl *ec = &br_ec_all_m31;
    t0 = au_mac_clock_ms();
    n = 0;
    do {
        ec->mulgen(point, scalar, sizeof scalar, BR_EC_secp256r1);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 1000);
    say(r, "p256 mulgen    %5lu ms\n", (unsigned long)(t / (unsigned)n));
    t0 = au_mac_clock_ms();
    n = 0;
    do {
        ec->mulgen(point, scalar, sizeof scalar, BR_EC_curve25519);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 1000);
    say(r, "x25519         %5lu ms\n", (unsigned long)(t / (unsigned)n));
}

/* ---- DNS and TCP through AuNetOps ---- */

static void probe(Run *r, const char *host, uint16_t port)
{
    const AuNetOps *ops = au_ot_net_ops(r->ot);
    uint32_t t0 = au_mac_clock_ms();
    uint32_t req = ops->resolve_start(ops->ctx, host);
    if (!req) {
        say(r, "dns %s: could not start\n", host);
        return;
    }
    AuNetAddr a;
    int rc;
    while ((rc = ops->resolve_poll(ops->ctx, req, &a)) == 0 && au_mac_clock_ms() - t0 < 20000)
        pass(r, 1);
    ops->resolve_cancel(ops->ctx, req);
    uint32_t t1 = au_mac_clock_ms();
    if (rc != 1) {
        say(r, "dns %s: failed after %lu ms\n", host, (unsigned long)(t1 - t0));
        return;
    }
    say(r, "dns %s: %u.%u.%u.%u in %lu ms\n", host, a.addr[0], a.addr[1], a.addr[2], a.addr[3],
        (unsigned long)(t1 - t0));
    uint32_t s = ops->connect(ops->ctx, &a, port);
    if (!s) {
        say(r, "tcp: could not start\n");
        return;
    }
    while ((rc = ops->connect_poll(ops->ctx, s)) == 0 && au_mac_clock_ms() - t1 < 20000)
        pass(r, 1);
    uint32_t t2 = au_mac_clock_ms();
    say(r, "tcp %s:%u: %s in %lu ms\n", host, port, rc == 1 ? "connected" : "failed", (unsigned long)(t2 - t1));
    ops->close(ops->ctx, s);
}

/* ---- Fetches ---- */

typedef struct Fetch {
    bool done;
    AuNetError err;
    int status;
    unsigned long bytes;
    uint32_t t_start, t_response, t_end;
    char tls[64];
} Fetch;

static void on_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *resp)
{
    (void)t;
    Fetch *f = ctx;
    f->status = resp->status;
    f->t_response = au_mac_clock_ms();
    if (resp->security.tls)
        snprintf(f->tls, sizeof f->tls, "TLS %s %s%s", resp->security.version == 0x0303 ? "1.2" : "?",
                 resp->security.cipher ? resp->security.cipher : "?", resp->security.resumed ? " resumed" : "");
}

static void on_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    (void)t;
    (void)data;
    Fetch *f = ctx;
    f->bytes += (unsigned long)len;
}

static void on_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    (void)t;
    Fetch *f = ctx;
    f->done = true;
    f->err = err;
    f->t_end = au_mac_clock_ms();
}

static void fetch(Run *r, const char *url)
{
    Fetch f;
    memset(&f, 0, sizeof f);
    AuHttpRequest rq;
    au_http_request_init(&rq, url);
    rq.user_activated = true;
    rq.total_timeout_ms = 120000;
    AuHttpCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = &f;
    cb.response = on_response;
    cb.data = on_data;
    cb.done = on_done;
    f.t_start = au_mac_clock_ms();
    au_http_start(r->net, &rq, &cb);
    au_http_request_free(&rq);
    while (!f.done)
        pass(r, 1);
    say(r, "%s\n", url);
    if (f.err != AU_NET_OK) {
        say(r, "  error %s after %lu ms\n", au_net_error_name(f.err), (unsigned long)(f.t_end - f.t_start));
        return;
    }
    say(r, "  %d %s, headers %lu ms, done %lu ms, %lu bytes\n", f.status, f.tls[0] ? f.tls : "plain",
        (unsigned long)(f.t_response - f.t_start), (unsigned long)(f.t_end - f.t_start), f.bytes);
}

static int load_urls(char **text, const char *urls[kMaxUrls])
{
    size_t len = 0;
    *text = read_beside("NetTest URLs", &len);
    int n = 0;
    if (*text) {
        char *p = *text;
        while (*p && n < kMaxUrls) {
            char *e = p + strcspn(p, "\r\n");
            char end = *e;
            *e = 0;
            if (*p && *p != '#')
                urls[n++] = p;
            if (!end)
                break;
            p = e + 1;
        }
    }
    if (n == 0) {
        for (size_t i = 0; i < sizeof kDefaultUrls / sizeof kDefaultUrls[0] && n < kMaxUrls; i++)
            urls[n++] = kDefaultUrls[i];
    }
    return n;
}

int main(void)
{
    Run r;
    memset(&r, 0, sizeof r);
    long ver = 0;
    Gestalt('otvr', &ver);
    say(&r, "NetTest: Aura network stack on Open Transport %08lX\n", (unsigned long)ver);
    say(&r, "time zone offset %ld s, UTC %ld\n", (long)au_mac_gmt_offset(), (long)au_mac_wall_time());

    uint32_t t0 = au_mac_clock_ms();
    r.entropy = au_mac_entropy_new();
    unsigned seed, startup, jitter, input;
    au_mac_entropy_sources(r.entropy, &seed, &startup, &jitter, &input);
    say(&r, "entropy: %u bits after start-up (seed %u, start-up jitter %u) in %lu ms\n",
        au_mac_entropy_bits(r.entropy), seed, startup, (unsigned long)(au_mac_clock_ms() - t0));

    long err = 0;
    t0 = au_mac_clock_ms();
    r.ot = au_ot_net_new(r.entropy, &err);
    if (!r.ot) {
        say(&r, "Open Transport did not start (error %ld)\n", err);
        write_log(&r);
        au_mac_entropy_free(r.entropy);
        return 1;
    }
    say(&r, "Open Transport started in %lu ms\n", (unsigned long)(au_mac_clock_ms() - t0));

    bench(&r);

    r.net = NULL;
    probe(&r, "example.com", 80);

    size_t rlen = 0;
    char *rdata = read_beside("aura-roots.bin", &rlen);
    if (!rdata) /* guest-push.sh would take a .bin file for MacBinary */
        rdata = read_beside("aura-roots", &rlen);
    AuRootStore *roots = NULL;
    if (rdata) {
        char msg[128];
        t0 = au_mac_clock_ms();
        roots = au_roots_load(rdata, rlen, msg, sizeof msg);
        au_free(rdata);
        if (roots)
            say(&r, "root store: %lu bytes, loaded in %lu ms\n", (unsigned long)rlen,
                (unsigned long)(au_mac_clock_ms() - t0));
        else
            say(&r, "root store: %s\n", msg);
    } else {
        say(&r, "root store: aura-roots.bin not found beside the application\n");
    }

    r.net = au_net_new(au_ot_net_ops(r.ot));
    if (roots)
        au_net_set_roots(r.net, roots);

    t0 = au_mac_clock_ms();
    while (au_mac_entropy_bits(r.entropy) < AU_MAC_ENTROPY_THRESHOLD)
        pass(&r, 1);
    au_mac_entropy_sources(r.entropy, &seed, &startup, &jitter, &input);
    say(&r, "entropy: %u bits after %lu ms more (loop jitter %u, input %u)\n", au_mac_entropy_bits(r.entropy),
        (unsigned long)(au_mac_clock_ms() - t0), jitter, input);

    char *urltext = NULL;
    const char *urls[kMaxUrls];
    int nurls = load_urls(&urltext, urls);
    for (int i = 0; i < nurls; i++)
        fetch(&r, urls[i]);
    /* The same again: pooled connections and resumed TLS sessions. */
    say(&r, "-- again --\n");
    for (int i = 0; i < nurls; i++)
        fetch(&r, urls[i]);

    AuOtNetStats st;
    au_ot_net_stats(r.ot, &st);
    say(&r, "lookups %u (%u failed), connects %u (%u failed), sent %lu, received %lu, last error %ld\n", st.lookups,
        st.lookups_failed, st.connects, st.connects_failed, st.bytes_sent, st.bytes_received, st.last_error);
    say(&r, "NetTest done. Press a key to quit.\n");
    write_log(&r);

    for (;;) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 10, NULL)) {
            au_mac_entropy_event(r.entropy, &ev);
            if (ev.what == keyDown || ev.what == mouseDown)
                break;
        }
        au_net_pump(r.net);
        au_ot_net_idle(r.ot);
    }
    au_net_free(r.net);
    if (roots)
        au_roots_free(roots);
    au_ot_net_free(r.ot);
    au_mac_entropy_free(r.entropy);
    if (urltext)
        au_free(urltext);
    au_free(r.log.buf);
    return 0;
}
