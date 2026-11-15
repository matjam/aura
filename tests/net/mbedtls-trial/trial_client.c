/* Mbed TLS trial client (docs/tls13-feasibility.md).
 *
 * Shows that Mbed TLS 4.1 can be driven the way Aura drives BearSSL: no
 * OS calls inside the library, the transport through two callbacks that
 * may say "would block", memory through a calloc/free pair, the clock and
 * the random source supplied by the embedder, and trust anchors handed
 * over one at a time by issuer name. It connects, completes a handshake,
 * sends one HEAD request, then connects again with the saved session.
 *
 * Usage: trial_client HOST PORT ROOTS [--tls12|--tls13] [--no-request]
 *
 * ROOTS is a file of trust anchors: for each, a 4-byte big-endian length
 * and the certificate in DER (make-roots.py writes it from a PEM bundle).
 * The sockets here are the harness's, not the library's. */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- Embedder services ---- */

/* Counting allocator: a size header in front of each block. */
static size_t mem_live, mem_peak, mem_blocks, mem_calls;

static void *count_calloc(size_t n, size_t size)
{
    size_t bytes = n * size;
    size_t *p = calloc(1, bytes + 16);
    if (!p)
        return NULL;
    p[0] = bytes;
    mem_live += bytes;
    mem_blocks++;
    mem_calls++;
    if (mem_live > mem_peak)
        mem_peak = mem_live;
    return (char *)p + 16;
}

static void count_free(void *ptr)
{
    if (!ptr)
        return;
    size_t *p = (size_t *)((char *)ptr - 16);
    mem_live -= p[0];
    mem_blocks--;
    free(p);
}

/* MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG: the library's only random source. In
 * Aura this would draw from a DRBG seeded by AuNetOps.entropy. */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context, uint8_t *output,
                                             size_t output_size, size_t *output_length)
{
    (void)context;
    arc4random_buf(output, output_size);
    *output_length = output_size;
    return PSA_SUCCESS;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT, MBEDTLS_PLATFORM_TIME_ALT and
 * MBEDTLS_PLATFORM_GMTIME_R_ALT: the embedder's clocks. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (mbedtls_ms_time_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static mbedtls_time_t wall_time(mbedtls_time_t *out)
{
    mbedtls_time_t t = (mbedtls_time_t)time(NULL);
    if (out)
        *out = t;
    return t;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    /* Days to civil date, without the C library's time zone code. */
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

/* ---- Trust anchors, parsed only when a chain names one as issuer ---- */

typedef struct Root {
    const uint8_t *der;
    size_t len;
    const uint8_t *subj; /* inside der */
    size_t subj_len;
} Root;

static Root *roots;
static size_t nroots;
static unsigned ca_cb_calls, ca_cb_parsed;

static int load_roots(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size)
        return -1;
    fclose(f);
    size_t off = 0;
    while (off + 4 <= (size_t)size) {
        size_t len = ((size_t)data[off] << 24) | ((size_t)data[off + 1] << 16) | ((size_t)data[off + 2] << 8) |
                     data[off + 3];
        off += 4;
        if (off + len > (size_t)size)
            return -1;
        /* Aura's roots.c already finds the subject without a parser; the
         * harness asks the library once and keeps only the offsets. */
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        int prc = mbedtls_x509_crt_parse_der_nocopy(&crt, data + off, len);
        if (prc != 0)
            printf("root %lu does not parse: -0x%04x\n", (unsigned long)nroots, (unsigned)-prc);
        if (prc == 0) {
            roots = realloc(roots, (nroots + 1) * sizeof *roots);
            roots[nroots].der = data + off;
            roots[nroots].len = len;
            roots[nroots].subj = crt.subject_raw.p;
            roots[nroots].subj_len = crt.subject_raw.len;
            nroots++;
        }
        mbedtls_x509_crt_free(&crt);
        off += len;
    }
    return 0;
}

static int ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates)
{
    (void)ctx;
    mbedtls_x509_crt *list = NULL;
    ca_cb_calls++;
    for (size_t i = 0; i < nroots; i++) {
        if (roots[i].subj_len != child->issuer_raw.len || memcmp(roots[i].subj, child->issuer_raw.p, roots[i].subj_len))
            continue;
        if (!list) {
            list = mbedtls_calloc(1, sizeof *list);
            if (!list)
                return MBEDTLS_ERR_X509_ALLOC_FAILED;
            mbedtls_x509_crt_init(list);
        }
        if (mbedtls_x509_crt_parse_der_nocopy(list, roots[i].der, roots[i].len) == 0)
            ca_cb_parsed++;
    }
    *candidates = list; /* the library frees it */
    return 0;
}

/* ---- Transport: a non-blocking socket behind the two callbacks ---- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    ssize_t n = send(*(int *)ctx, buf, len, 0);
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    ssize_t n = recv(*(int *)ctx, buf, len, 0);
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    return (int)n; /* 0: the peer closed */
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0)
        return -1;
    freeaddrinfo(res);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static unsigned would_block;

/* Waits until the socket is ready the way the last call asked. */
static int wait_for(int fd, int rc)
{
    struct pollfd p = {fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN, 0};
    would_block++;
    return poll(&p, 1, 15000) > 0;
}

/* ---- One connection ---- */

typedef struct Saved {
    unsigned char data[8192];
    size_t len;
} Saved;

static int run(const char *host, const char *port, int minv, int maxv, int request, Saved *saved, int resume)
{
    int rc, ok = 0;
    size_t base = mem_live;
    mem_peak = mem_live;
    would_block = 0;
    ca_cb_calls = ca_cb_parsed = 0;

    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_session sess;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_session_init(&sess);
    static const char *alpn[] = {"http/1.1", NULL};

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        printf("  tcp connect failed\n");
        return 0;
    }
    if ((rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        goto done;
    /* OPTIONAL: the handshake completes on a certificate error and the
     * embedder decides (the error page, the user's exception, chain
     * completion from the intermediates pool). Nothing is sent before
     * the verify result is read. */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_cb(&conf, ca_cb, NULL);
    mbedtls_ssl_conf_min_tls_version(&conf, minv);
    mbedtls_ssl_conf_max_tls_version(&conf, maxv);
    mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
    if ((rc = mbedtls_ssl_setup(&ssl, &conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&ssl, host)) != 0)
        goto done;
    if (resume && saved->len) {
        if ((rc = mbedtls_ssl_session_load(&sess, saved->data, saved->len)) != 0 ||
            (rc = mbedtls_ssl_set_session(&ssl, &sess)) != 0)
            goto done;
    }
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);

    mbedtls_ms_time_t t0 = mbedtls_ms_time();
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto done;
        if (!wait_for(fd, rc)) {
            rc = -1;
            goto done;
        }
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
    int chain = 0;
    for (const mbedtls_x509_crt *c = mbedtls_ssl_get_peer_cert(&ssl); c; c = c->next)
        chain++;
    const char *proto = mbedtls_ssl_get_alpn_protocol(&ssl);
    printf("  %s, %s, ALPN %s, %d ms, %u waits\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl),
           proto ? proto : "none", (int)(mbedtls_ms_time() - t0), would_block);
    printf("  certificates sent: %d%s; verify flags 0x%x; anchor lookups %u, anchors parsed %u\n", chain,
           chain == 0 ? " (resumed)" : "", (unsigned)flags, ca_cb_calls, ca_cb_parsed);
    if (flags) {
        char info[512];
        mbedtls_x509_crt_verify_info(info, sizeof info, "    ", flags);
        printf("%s", info);
    }
    printf("  memory: peak %lu bytes in the handshake, %lu bytes while open, %lu blocks\n",
           (unsigned long)(mem_peak - base), (unsigned long)(mem_live - base), (unsigned long)mem_blocks);
    /* A resumed TLS 1.3 handshake carries no certificate: the library
     * reports that verification was skipped. */
    ok = flags == 0 || (resume && chain == 0 && flags == MBEDTLS_X509_BADCERT_SKIP_VERIFY);

    if (ok && request) {
        char req[512];
        int n = snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: aura-tls-trial\r\nConnection: close\r\n\r\n",
                         host);
        while ((rc = mbedtls_ssl_write(&ssl, (const unsigned char *)req, (size_t)n)) <= 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
                goto done;
            wait_for(fd, rc);
        }
        unsigned char buf[2048];
        size_t got = 0;
        int tickets = 0;
        for (;;) {
            rc = mbedtls_ssl_read(&ssl, buf + got, sizeof buf - 1 - got);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!wait_for(fd, rc))
                    break;
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                tickets++; /* TLS 1.3 sends tickets after the handshake */
                continue;
            }
            if (rc <= 0)
                break;
            got += (size_t)rc;
            if (got >= sizeof buf - 1 || memmem(buf, got, "\r\n\r\n", 4))
                break;
        }
        buf[got] = 0;
        char *eol = strstr((char *)buf, "\r\n");
        if (eol)
            *eol = 0;
        printf("  response: %s (%d ticket message(s))\n", got ? (char *)buf : "(none)", tickets);
        rc = 0;
    }
    /* The session, as bytes the embedder can keep (AuTlsSession). */
    mbedtls_ssl_session_free(&sess);
    mbedtls_ssl_session_init(&sess);
    if (mbedtls_ssl_get_session(&ssl, &sess) == 0 &&
        mbedtls_ssl_session_save(&sess, saved->data, sizeof saved->data, &saved->len) == 0)
        printf("  saved session: %lu bytes\n", (unsigned long)saved->len);
    else
        saved->len = 0;
    rc = 0;
done:
    if (rc != 0)
        printf("  failed: -0x%04x\n", (unsigned)-rc);
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_session_free(&sess);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    close(fd);
    if (mem_live != base)
        printf("  LEAK: %lu bytes\n", (unsigned long)(mem_live - base));
    return ok && rc == 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: trial_client HOST PORT ROOTS [--tls12|--tls13] [--no-request]\n");
        return 2;
    }
    int minv = MBEDTLS_SSL_VERSION_TLS1_2, maxv = MBEDTLS_SSL_VERSION_TLS1_3, request = 1;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--tls12"))
            maxv = MBEDTLS_SSL_VERSION_TLS1_2;
        else if (!strcmp(argv[i], "--tls13"))
            minv = MBEDTLS_SSL_VERSION_TLS1_3;
        else if (!strcmp(argv[i], "--no-request"))
            request = 0;
    }
    mbedtls_platform_set_calloc_free(count_calloc, count_free);
    mbedtls_platform_set_time(wall_time);
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("psa_crypto_init failed\n");
        return 1;
    }
    size_t after_init = mem_live;
    if (load_roots(argv[3]) != 0) {
        printf("cannot read %s\n", argv[3]);
        return 1;
    }
    printf("%s:%s (%lu roots; library holds %lu bytes after start, %lu after the roots index)\n", argv[1], argv[2],
           (unsigned long)nroots, (unsigned long)after_init, (unsigned long)mem_live);
    Saved saved;
    saved.len = 0;
    printf(" full handshake\n");
    int ok = run(argv[1], argv[2], minv, maxv, request, &saved, 0);
    if (ok && saved.len) {
        printf(" resumed handshake\n");
        ok = run(argv[1], argv[2], minv, maxv, request, &saved, 1);
    }
    return ok ? 0 : 1;
}
