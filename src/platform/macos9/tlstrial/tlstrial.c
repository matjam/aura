/* TlsTrial: Mbed TLS 4.1 on Mac OS 9, in a console window
 * (docs/tls13-feasibility.md). Not part of Aura; built only when
 * AURA_MBEDTLS_DIR and AURA_MBEDTLS_LIB are given to CMake.
 *
 * It times the Mbed TLS primitives a handshake uses (the same list as
 * NetTest prints for BearSSL), then connects to each host in the list
 * through Aura's Open Transport sockets (AuNetOps), completes a TLS
 * handshake with certificate verification, sends a HEAD request, and
 * connects again with the saved session. It prints the protocol version,
 * the cipher suite, the time and the memory the library allocated.
 *
 * Files beside the application:
 *   trial-roots      trust anchors: for each, a 4-byte big-endian length
 *                    and the certificate in DER
 *                    (tests/net/mbedtls-trial/make-roots.py)
 *   TlsTrial Hosts   optional, one "host" or "host 12" or "host 13" per
 *                    line (12 and 13 limit the protocol version)
 *
 * The library makes no OS call: sockets, clocks, entropy and memory come
 * from the functions in this file. */
#include <Multiverse.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "bearssl.h"

#include "core/core.h"
#include "platform/platform.h"

#include "../clock_mac.h"
#include "../entropy.h"
#include "../net_ot.h"

static AuMacEntropy *g_entropy;
static AuOtNet *g_ot;
static const AuNetOps *g_ops;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void pass(long sleep)
{
    EventRecord ev;
    if (WaitNextEvent(everyEvent, &ev, sleep, NULL))
        au_mac_entropy_event(g_entropy, &ev);
    au_mac_entropy_idle(g_entropy);
    if (g_ot)
        au_ot_net_idle(g_ot);
}

/* ---- What the library asks of the embedder ---- */

static size_t mem_live, mem_peak, mem_blocks;

static void *count_calloc(size_t n, size_t size)
{
    size_t bytes = n * size;
    size_t *p = au_alloc(bytes + 8);
    memset(p, 0, bytes + 8);
    p[0] = bytes;
    mem_live += bytes;
    mem_blocks++;
    if (mem_live > mem_peak)
        mem_peak = mem_live;
    return (char *)p + 8;
}

static void count_free(void *ptr)
{
    if (!ptr)
        return;
    size_t *p = (size_t *)((char *)ptr - 8);
    mem_live -= p[0];
    mem_blocks--;
    au_free(p);
}

/* The library's random source: an HMAC-DRBG seeded once from the entropy
 * pool. (Drawing from the pool on every call made each elliptic curve
 * operation pay for a pool extraction: the library randomizes
 * coordinates.) The DRBG here is BearSSL's, which the engine library
 * still carries; a real port would let the library's own PSA generator
 * draw its seed through MBEDTLS_PSA_DRIVER_GET_ENTROPY. */
static br_hmac_drbg_context g_drbg;
static bool g_drbg_ready;

psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context, uint8_t *output,
                                             size_t output_size, size_t *output_length)
{
    (void)context;
    if (!g_drbg_ready) {
        uint8_t seed[32];
        if (!g_ops->entropy(g_ops->ctx, seed, sizeof seed))
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        br_hmac_drbg_init(&g_drbg, &br_sha256_vtable, seed, sizeof seed);
        g_drbg_ready = true;
    }
    br_hmac_drbg_generate(&g_drbg, output, output_size);
    *output_length = output_size;
    return PSA_SUCCESS;
}

mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)au_mac_clock_ms(); }

static mbedtls_time_t wall_time(mbedtls_time_t *out)
{
    mbedtls_time_t t = (mbedtls_time_t)au_mac_wall_time();
    if (out)
        *out = t;
    return t;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    int64_t t = (int64_t)*tt, days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    memset(tm_buf, 0, sizeof *tm_buf);
    tm_buf->tm_year = (int)(yoe + era * 400 + (m <= 2)) - 1900;
    tm_buf->tm_mon = (int)m - 1;
    tm_buf->tm_mday = (int)d;
    tm_buf->tm_hour = (int)(rem / 3600);
    tm_buf->tm_min = (int)(rem % 3600 / 60);
    tm_buf->tm_sec = (int)(rem % 60);
    return tm_buf;
}

/* ---- Files beside the application ---- */

static char *read_beside(const char *fname, size_t *len)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec app, spec;
    Str255 name, p;
    if (GetCurrentProcess(&psn) != noErr)
        return NULL;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processName = name;
    info.processAppSpec = &app;
    if (GetProcessInformation(&psn, &info) != noErr)
        return NULL;
    size_t n = strlen(fname);
    p[0] = (unsigned char)n;
    memcpy(p + 1, fname, n);
    if (FSMakeFSSpec(app.vRefNum, app.parID, p, &spec) != noErr)
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

/* ---- Trust anchors ---- */

typedef struct Root {
    const uint8_t *der, *subj;
    size_t len, subj_len;
} Root;

static Root *roots;
static size_t nroots;
static unsigned ca_parsed;

static void load_roots(const uint8_t *data, size_t size)
{
    size_t off = 0;
    while (off + 4 <= size) {
        size_t len = ((size_t)data[off] << 24) | ((size_t)data[off + 1] << 16) | ((size_t)data[off + 2] << 8) |
                     data[off + 3];
        off += 4;
        if (off + len > size)
            break;
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        if (mbedtls_x509_crt_parse_der_nocopy(&crt, data + off, len) == 0) {
            roots = au_realloc(roots, (nroots + 1) * sizeof *roots);
            roots[nroots].der = data + off;
            roots[nroots].len = len;
            roots[nroots].subj = crt.subject_raw.p;
            roots[nroots].subj_len = crt.subject_raw.len;
            nroots++;
        }
        mbedtls_x509_crt_free(&crt);
        off += len;
    }
}

static int ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates)
{
    (void)ctx;
    mbedtls_x509_crt *list = NULL;
    for (size_t i = 0; i < nroots; i++) {
        if (roots[i].subj_len != child->issuer_raw.len || memcmp(roots[i].subj, child->issuer_raw.p, roots[i].subj_len))
            continue;
        if (!list) {
            list = mbedtls_calloc(1, sizeof *list);
            mbedtls_x509_crt_init(list);
        }
        if (mbedtls_x509_crt_parse_der_nocopy(list, roots[i].der, roots[i].len) == 0)
            ca_parsed++;
    }
    *candidates = list;
    return 0;
}

/* ---- Transport: AuNetOps behind the library's two callbacks ---- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    long n = g_ops->send(g_ops->ctx, *(uint32_t *)ctx, buf, len);
    if (n == 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return n < 0 ? -1 : (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    long n = g_ops->recv(g_ops->ctx, *(uint32_t *)ctx, buf, len);
    if (n == AU_NETIO_WOULD_BLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == AU_NETIO_CLOSED)
        return 0;
    return n < 0 ? -1 : (int)n;
}

static uint32_t tcp_connect(const char *host)
{
    uint32_t t0 = au_mac_clock_ms();
    uint32_t req = g_ops->resolve_start(g_ops->ctx, host);
    if (!req)
        return 0;
    AuNetAddr a;
    int rc;
    while ((rc = g_ops->resolve_poll(g_ops->ctx, req, &a)) == 0 && au_mac_clock_ms() - t0 < 20000)
        pass(1);
    g_ops->resolve_cancel(g_ops->ctx, req);
    if (rc != 1)
        return 0;
    uint32_t s = g_ops->connect(g_ops->ctx, &a, 443);
    if (!s)
        return 0;
    while ((rc = g_ops->connect_poll(g_ops->ctx, s)) == 0 && au_mac_clock_ms() - t0 < 20000)
        pass(1);
    if (rc != 1) {
        g_ops->close(g_ops->ctx, s);
        return 0;
    }
    return s;
}

typedef struct Saved {
    unsigned char data[8192];
    size_t len;
} Saved;

static Saved saved;

static int run(const char *host, int minv, int maxv, int resume)
{
    int rc, ok = 0;
    size_t base = mem_live;
    mem_peak = mem_live;
    ca_parsed = 0;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_session sess;
    static const char *alpn[] = {"http/1.1", NULL};

    uint32_t sock = tcp_connect(host);
    if (!sock) {
        say("  tcp connect failed\n");
        return 0;
    }
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_session_init(&sess);
    if ((rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        goto done;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_cb(&conf, ca_cb, NULL);
    mbedtls_ssl_conf_min_tls_version(&conf, minv);
    mbedtls_ssl_conf_max_tls_version(&conf, maxv);
    mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
    if ((rc = mbedtls_ssl_setup(&ssl, &conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&ssl, host)) != 0)
        goto done;
    if (resume && saved.len) {
        if ((rc = mbedtls_ssl_session_load(&sess, saved.data, saved.len)) != 0 ||
            (rc = mbedtls_ssl_set_session(&ssl, &sess)) != 0)
            goto done;
    }
    mbedtls_ssl_set_bio(&ssl, &sock, bio_send, bio_recv, NULL);

    uint32_t t0 = au_mac_clock_ms(), busy = 0, tb;
    for (;;) {
        tb = au_mac_clock_ms();
        rc = mbedtls_ssl_handshake(&ssl);
        busy += au_mac_clock_ms() - tb;
        if (rc == 0)
            break;
        if ((rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) || au_mac_clock_ms() - t0 > 30000)
            goto done;
        pass(1);
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
    int chain = 0;
    for (const mbedtls_x509_crt *c = mbedtls_ssl_get_peer_cert(&ssl); c; c = c->next)
        chain++;
    say("  %s %s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
    say("  handshake %lu ms (%lu ms in the library), %d certs, flags 0x%lx, anchors parsed %u\n",
        (unsigned long)(au_mac_clock_ms() - t0), (unsigned long)busy, chain, (unsigned long)flags, ca_parsed);
    say("  memory: peak %lu, open %lu bytes\n", (unsigned long)(mem_peak - base), (unsigned long)(mem_live - base));
    ok = flags == 0 || (resume && chain == 0 && flags == MBEDTLS_X509_BADCERT_SKIP_VERIFY);
    if (ok) {
        char req[300];
        int n = snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: aura-tls-trial\r\nConnection: close\r\n\r\n", host);
        while ((rc = mbedtls_ssl_write(&ssl, (const unsigned char *)req, (size_t)n)) <= 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
                goto done;
            pass(1);
        }
        static unsigned char buf[1024];
        size_t got = 0;
        t0 = au_mac_clock_ms();
        while (au_mac_clock_ms() - t0 < 15000) {
            rc = mbedtls_ssl_read(&ssl, buf + got, sizeof buf - 1 - got);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                pass(1);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;
            if (rc <= 0)
                break;
            got += (size_t)rc;
            buf[got] = 0;
            if (got >= sizeof buf - 1 || strstr((char *)buf, "\r\n"))
                break;
        }
        buf[got] = 0;
        char *eol = strstr((char *)buf, "\r\n");
        if (eol)
            *eol = 0;
        say("  response: %s\n", got ? (char *)buf : "(none)");
        mbedtls_ssl_session_free(&sess);
        mbedtls_ssl_session_init(&sess);
        if (mbedtls_ssl_get_session(&ssl, &sess) != 0 ||
            mbedtls_ssl_session_save(&sess, saved.data, sizeof saved.data, &saved.len) != 0)
            saved.len = 0;
    }
    rc = 0;
done:
    if (rc != 0)
        say("  failed: -0x%04x\n", (unsigned)-rc);
    mbedtls_ssl_session_free(&sess);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    g_ops->close(g_ops->ctx, sock);
    return ok && rc == 0;
}

/* ---- Timings of the primitives, through the PSA interface ---- */

static void tenths(const char *name, uint32_t t, int n)
{
    say("%-15s%5lu.%lu ms\n", name, (unsigned long)(t / (unsigned)n), (unsigned long)(t * 10 / (unsigned)n % 10));
}

static void bench(void)
{
    static uint8_t buf[16384 + 16], out[16384 + 32];
    uint8_t key[32], nonce[12], tag[64];
    size_t olen;
    uint32_t t0, t;
    int n;
    memset(buf, 0x5A, sizeof buf);
    memset(key, 1, sizeof key);
    memset(nonce, 2, sizeof nonce);

    t0 = au_mac_clock_ms();
    n = 0;
    do {
        psa_hash_compute(PSA_ALG_SHA_256, buf, 16384, tag, sizeof tag, &olen);
        n++;
    } while ((t = au_mac_clock_ms() - t0) < 500);
    say("sha256         %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));

    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&at, PSA_ALG_GCM);
    psa_set_key_type(&at, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&at, 128);
    if (psa_import_key(&at, key, 16, &id) == PSA_SUCCESS) {
        t0 = au_mac_clock_ms();
        n = 0;
        do {
            psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12, NULL, 0, buf, 16384, out, sizeof out, &olen);
            n++;
        } while ((t = au_mac_clock_ms() - t0) < 500);
        say("aes128-gcm     %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));
        psa_destroy_key(id);
    }
    at = psa_key_attributes_init();
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&at, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_type(&at, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&at, 256);
    if (psa_import_key(&at, key, 32, &id) == PSA_SUCCESS) {
        t0 = au_mac_clock_ms();
        n = 0;
        do {
            psa_aead_encrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, 12, NULL, 0, buf, 16384, out, sizeof out, &olen);
            n++;
        } while ((t = au_mac_clock_ms() - t0) < 500);
        say("chacha20-poly  %5lu KB/s\n", (unsigned long)(n * 16000UL / (t ? t : 1)));
        psa_destroy_key(id);
    }

    /* Key exchange: make a key pair, then the shared secret with a peer
     * key (as a client does once per handshake). */
    static const struct {
        const char *gen, *dh;
        psa_ecc_family_t fam;
        size_t bits;
    } kx[] = {{"x25519 keygen", "x25519 ecdh", PSA_ECC_FAMILY_MONTGOMERY, 255},
              {"p256 keygen", "p256 ecdh", PSA_ECC_FAMILY_SECP_R1, 256}};
    uint8_t peer[80], pub[80];
    size_t peer_len = 0, pub_len = 0;
    for (int k = 0; k < 2; k++) {
        psa_key_id_t a = 0, b = 0;
        at = psa_key_attributes_init();
        psa_set_key_usage_flags(&at, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&at, PSA_ALG_ECDH);
        psa_set_key_type(&at, PSA_KEY_TYPE_ECC_KEY_PAIR(kx[k].fam));
        psa_set_key_bits(&at, kx[k].bits);
        if (psa_generate_key(&at, &b) != PSA_SUCCESS || psa_export_public_key(b, peer, sizeof peer, &peer_len) != PSA_SUCCESS) {
            say("%s: key generation failed\n", kx[k].gen);
            continue;
        }
        t0 = au_mac_clock_ms();
        n = 0;
        do {
            if (a)
                psa_destroy_key(a);
            psa_generate_key(&at, &a);
            psa_export_public_key(a, pub, sizeof pub, &pub_len);
            n++;
        } while ((t = au_mac_clock_ms() - t0) < 1000);
        tenths(kx[k].gen, t, n);
        t0 = au_mac_clock_ms();
        n = 0;
        do {
            psa_raw_key_agreement(PSA_ALG_ECDH, a, peer, peer_len, out, 64, &olen);
            n++;
        } while ((t = au_mac_clock_ms() - t0) < 1000);
        tenths(kx[k].dh, t, n);
        if (k == 1) {
            /* ECDSA verification with the P-256 public key; the signature
             * is not valid, the work is the same. */
            psa_key_id_t v = 0;
            at = psa_key_attributes_init();
            psa_set_key_usage_flags(&at, PSA_KEY_USAGE_VERIFY_HASH);
            psa_set_key_algorithm(&at, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
            psa_set_key_type(&at, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
            if (psa_import_key(&at, pub, pub_len, &v) == PSA_SUCCESS) {
                memset(out, 0x21, 64);
                t0 = au_mac_clock_ms();
                n = 0;
                do {
                    psa_verify_hash(v, PSA_ALG_ECDSA(PSA_ALG_SHA_256), key, 32, out, 64);
                    n++;
                } while ((t = au_mac_clock_ms() - t0) < 1000);
                tenths("ecdsa p256 vfy", t, n);
                psa_destroy_key(v);
            }
        }
        psa_destroy_key(a);
        psa_destroy_key(b);
    }

    /* RSA verification: the public-key operation dominates. The modulus
     * is not a product of two primes; the work is the same. */
    for (int bits = 2048; bits <= 4096; bits *= 2) {
        static uint8_t der[540], sig[512];
        size_t len = (size_t)bits / 8, d = 0;
        /* RSAPublicKey ::= SEQUENCE { INTEGER n, INTEGER 65537 } */
        size_t body = 4 + 1 + len + 5;
        der[d++] = 0x30;
        der[d++] = 0x82;
        der[d++] = (uint8_t)(body >> 8);
        der[d++] = (uint8_t)body;
        der[d++] = 0x02;
        der[d++] = 0x82;
        der[d++] = (uint8_t)((len + 1) >> 8);
        der[d++] = (uint8_t)(len + 1);
        der[d++] = 0;
        for (size_t i = 0; i < len; i++)
            der[d++] = (uint8_t)(0xC1 + i * 7);
        der[d - len] |= 0x80;
        der[d - 1] |= 1;
        der[d++] = 0x02;
        der[d++] = 0x03;
        der[d++] = 1;
        der[d++] = 0;
        der[d++] = 1;
        psa_key_id_t v = 0;
        at = psa_key_attributes_init();
        psa_set_key_usage_flags(&at, PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&at, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256));
        psa_set_key_type(&at, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
        psa_status_t st = psa_import_key(&at, der, d, &v);
        if (st != PSA_SUCCESS) {
            say("rsa%d: import failed (%ld)\n", bits, (long)st);
            continue;
        }
        memset(sig, 0x42, len);
        t0 = au_mac_clock_ms();
        n = 0;
        do {
            psa_verify_hash(v, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256), key, 32, sig, len);
            n++;
        } while ((t = au_mac_clock_ms() - t0) < 1000);
        char name[24];
        snprintf(name, sizeof name, "rsa%d pss vfy", bits);
        tenths(name, t, n);
        psa_destroy_key(v);
    }
}

static void wait_key(void)
{
    for (;;) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 10, NULL)) {
            au_mac_entropy_event(g_entropy, &ev);
            if (ev.what == keyDown || ev.what == mouseDown)
                break;
        }
        au_ot_net_idle(g_ot);
    }
}

int main(void)
{
    say("TlsTrial: Mbed TLS %s on Mac OS 9\n", MBEDTLS_VERSION_STRING);
    g_entropy = au_mac_entropy_new();
    long err = 0;
    g_ot = au_ot_net_new(g_entropy, &err);
    if (!g_ot) {
        say("Open Transport did not start (error %ld)\n", err);
        return 1;
    }
    g_ops = au_ot_net_ops(g_ot);
    mbedtls_platform_set_calloc_free(count_calloc, count_free);
    mbedtls_platform_set_time(wall_time);
    uint32_t t0 = au_mac_clock_ms();
    uint8_t probe[8];
    while (!g_ops->entropy(g_ops->ctx, probe, sizeof probe))
        pass(1);
    say("entropy ready after %lu ms\n", (unsigned long)(au_mac_clock_ms() - t0));
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        say("psa_crypto_init failed (%ld)\n", (long)st);
        return 1;
    }
    bench();
    say("Press a key for the handshakes.\n");
    wait_key();

    size_t rlen = 0;
    char *rdata = read_beside("trial-roots", &rlen);
    if (rdata) {
        t0 = au_mac_clock_ms();
        load_roots((const uint8_t *)rdata, rlen);
        say("roots: %lu indexed in %lu ms; library holds %lu bytes\n", (unsigned long)nroots,
            (unsigned long)(au_mac_clock_ms() - t0), (unsigned long)mem_live);
    } else {
        say("no trial-roots file: certificates will not verify\n");
    }

    static const char *const defaults[] = {"example.com 13", "example.com 12", "en.wikipedia.org 13", "www.reddit.com 13"};
    const char *lines[16];
    int nlines = 0;
    size_t hlen = 0;
    char *hosts = read_beside("TlsTrial Hosts", &hlen);
    for (char *p = hosts; p && *p && nlines < 16;) {
        char *e = p + strcspn(p, "\r\n");
        char end = *e;
        *e = 0;
        if (*p && *p != '#')
            lines[nlines++] = p;
        if (!end)
            break;
        p = e + 1;
    }
    if (nlines == 0)
        for (int i = 0; i < 4; i++)
            lines[nlines++] = defaults[i];
    for (int i = 0; i < nlines; i++) {
        char host[128];
        int ver = 0;
        sscanf(lines[i], "%127s %d", host, &ver);
        int minv = ver == 13 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2;
        int maxv = ver == 12 ? MBEDTLS_SSL_VERSION_TLS1_2 : MBEDTLS_SSL_VERSION_TLS1_3;
        say("%s\n", lines[i]);
        saved.len = 0;
        if (run(host, minv, maxv, 0) && saved.len) {
            say(" resumed (%lu byte session)\n", (unsigned long)saved.len);
            run(host, minv, maxv, 1);
        }
    }
    say("TlsTrial done. Press a key to quit.\n");
    wait_key();
    /* Close Open Transport before leaving: with providers open at exit
     * the guest froze (CLAUDE.md, Browser chrome). */
    au_ot_net_free(g_ot);
    au_mac_entropy_free(g_entropy);
    return 0;
}
