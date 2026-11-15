/* TLS client glue over Mbed TLS. See tls.h.
 *
 * The library reads and writes ciphertext through two callbacks; the
 * stack hands ciphertext and plaintext over in buffers (au_tls_in,
 * au_tls_out, au_tls_read, au_tls_write). Four staging buffers in AuTls
 * join the two: the callbacks move bytes between the library and the
 * staging buffers, and say "would block" when one is empty or full.
 *
 * Certificates. The library verifies the chain during the handshake in
 * "optional" mode: a certificate error does not stop the handshake, and
 * finish_handshake decides before any application data moves. That is
 * where Aura's own rules live: the chain kept for the certificate viewer
 * and the error pages, completing a chain the server sent short from the
 * intermediates pool, and the user's exception for one leaf certificate.
 * Trust anchors reach the library through ca_cb, by issuer name, from the
 * root store only: pool certificates are candidates for the path and
 * never anchors. */
#include "tls.h"

#include "roots.h"
#include "tls_platform.h"

#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <string.h>

#define CERT_KEEP_MAX 16384 /* bytes of each chain certificate kept for the viewer and path building */
#define IN_CAP 4096         /* ciphertext from the socket, waiting for the library */
#define OUT_CAP 4608        /* ciphertext from the library, waiting for the socket: one 4 KB record */
#define APP_CAP 4096        /* decrypted data, waiting for the stack */
#define WRITE_CAP 4096      /* plaintext from the stack, waiting to be encrypted (MBEDTLS_SSL_OUT_CONTENT_LEN) */

struct AuTls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_session resume;
    bool have_resume;
    const AuRootStore *roots;
    const AuCertPool *pool;
    char host[256];
    int64_t now;
    AuCertInfo info;
    bool failed, closed, open, eof;
    int error;
    bool spoke_tls;   /* the peer's first bytes were a TLS record header */
    uint8_t first[5]; /* those bytes */
    size_t nfirst;
    /* The server's certificates, in the order sent (at most
     * AU_CERT_MAX_CHAIN; der[i] NULL with der_len 0 when one was too
     * large), copied when the library verifies the chain. */
    uint8_t *der[AU_CERT_MAX_CHAIN];
    size_t der_len[AU_CERT_MAX_CHAIN];
    int nder, nsent;
    bool saw_chain;
    /* The user's session exception (au_tls_allow_cert). */
    bool allow;
    uint8_t allow_sha256[32];
    bool resumed;
    bool new_session; /* a session (TLS 1.3: a ticket) arrived and was not taken yet */
    /* Staging. wr exists only while there is plaintext to send, so an
     * idle connection does not hold it. */
    uint8_t *in, *out, *app, *wr;
    size_t in_len, in_pos, out_len, out_pos, app_len, app_pos, wr_len;
    size_t wr_pending; /* bytes of wr given to the library in a write that must be repeated */
    bool wr_flush;
    const char *alpn[2];
};

/* Aura's order: ChaCha20 first (no AES instructions on a G4), then
 * AES-128, then AES-256. Every suite has forward secrecy. */
static const int suites[] = {
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    0,
};

/* The same with RSA key exchange at the end (au_tls_allow_legacy_kex):
 * no forward secrecy, for old servers that offer nothing else. A server
 * with any suite above takes that one. */
static const int suites_legacy[] = {
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
    0,
};

/* The first group is the one a key share is sent for; a server that
 * wants another asks with a HelloRetryRequest. */
static const uint16_t groups[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_NONE,
};

/* Handshake signatures. No SHA-1. */
static const uint16_t sig_algs[] = {
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256, MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP521R1_SHA512, MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA384,    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA512,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256,       MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512,       MBEDTLS_TLS1_3_SIG_NONE,
};

/* ---- The library's transport callbacks ---- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    AuTls *t = ctx;
    if (t->out_pos == t->out_len)
        t->out_pos = t->out_len = 0;
    size_t room = OUT_CAP - t->out_len;
    if (room == 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (len > room)
        len = room;
    memcpy(t->out + t->out_len, buf, len);
    t->out_len += len;
    return (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    AuTls *t = ctx;
    size_t have = t->in_len - t->in_pos;
    if (have == 0)
        return t->eof ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    if (len > have)
        len = have;
    memcpy(buf, t->in + t->in_pos, len);
    t->in_pos += len;
    if (t->in_pos == t->in_len)
        t->in_pos = t->in_len = 0;
    return (int)len;
}

/* ---- Trust anchors and the chain as sent ---- */

/* The library asks for the certificates that may have signed child. Only
 * root store certificates are offered, and only those with the issuer's
 * name, so one or two roots are parsed per handshake. The library frees
 * the list. */
static int ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates)
{
    AuTls *t = ctx;
    mbedtls_x509_crt *list = NULL;
    const uint8_t *der;
    size_t len, pos = 0;
    *candidates = NULL;
    while (au_roots_next_by_subject(t->roots, child->issuer_raw.p, child->issuer_raw.len, &pos, &der, &len)) {
        if (!list) {
            list = mbedtls_calloc(1, sizeof *list);
            if (!list)
                return MBEDTLS_ERR_X509_ALLOC_FAILED;
            mbedtls_x509_crt_init(list);
        }
        /* The store's bytes outlive every connection: no copy. A root
         * the library cannot parse is not a candidate. */
        int rc = mbedtls_x509_crt_parse_der_nocopy(list, der, len);
        if (rc == MBEDTLS_ERR_X509_ALLOC_FAILED) {
            mbedtls_x509_crt_free(list);
            mbedtls_free(list);
            return rc;
        }
    }
    *candidates = list;
    return 0;
}

static void drop_chain(AuTls *t)
{
    for (int i = 0; i < AU_CERT_MAX_CHAIN; i++) {
        au_free(t->der[i]);
        t->der[i] = NULL;
        t->der_len[i] = 0;
    }
    t->nder = t->nsent = 0;
}

/* Called by the library for each certificate of the path it built, the
 * leaf (depth 0) last. The leaf heads the list the server sent: copy
 * that list. Returns 0 always: the verdict is read after the handshake. */
static int vrfy_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    AuTls *t = ctx;
    (void)flags;
    if (depth != 0 || t->saw_chain)
        return 0;
    t->saw_chain = true;
    drop_chain(t);
    for (const mbedtls_x509_crt *c = crt; c; c = c->next) {
        if (t->nder < AU_CERT_MAX_CHAIN) {
            int k = t->nder++;
            if (c->raw.len <= CERT_KEEP_MAX && (t->der[k] = au_try_alloc(c->raw.len ? c->raw.len : 1)) != NULL) {
                memcpy(t->der[k], c->raw.p, c->raw.len);
                t->der_len[k] = c->raw.len;
            }
        }
        t->nsent++;
    }
    return 0;
}

/* ---- Errors ---- */

/* The one error reported for a set of verification flags. A certificate
 * that is expired or for another site is reported as that even when its
 * chain is also untrusted, as before. */
static int cert_error(uint32_t flags)
{
    if (flags == 0)
        return 0;
    if (flags == (uint32_t)-1)
        return AU_TLS_ERR_CERT_MALFORMED;
    if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
        return AU_TLS_ERR_CERT_EXPIRED;
    if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
        return AU_TLS_ERR_CERT_BAD_NAME;
    if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
        return AU_TLS_ERR_CERT_NOT_TRUSTED;
    if (flags & MBEDTLS_X509_BADCERT_BAD_KEY)
        return AU_TLS_ERR_CERT_WEAK_KEY;
    if (flags & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK))
        return AU_TLS_ERR_CERT_UNSUPPORTED;
    if (flags & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE | MBEDTLS_X509_BADCERT_NS_CERT_TYPE))
        return AU_TLS_ERR_CERT_KEY_USAGE;
    if (flags & MBEDTLS_X509_BADCERT_MISSING)
        return AU_TLS_ERR_CERT_EMPTY_CHAIN;
    return AU_TLS_ERR_CERT_MALFORMED;
}

static int lib_error(int rc)
{
    switch (rc) {
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE: return AU_TLS_ERR_ALERT;
    case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION: return AU_TLS_ERR_VERSION;
    case MBEDTLS_ERR_SSL_INVALID_MAC: return AU_TLS_ERR_BAD_MAC;
    case MBEDTLS_ERR_SSL_CONN_EOF: return AU_TLS_ERR_IO;
    case MBEDTLS_ERR_SSL_ALLOC_FAILED: return AU_TLS_ERR_MEMORY;
    case MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER: return AU_TLS_ERR_ILLEGAL;
    case MBEDTLS_ERR_SSL_NO_RNG: return AU_TLS_ERR_NO_RANDOM;
    case MBEDTLS_ERR_SSL_BAD_CERTIFICATE: return AU_TLS_ERR_CERT_MALFORMED;
    default:
        /* X.509 parse errors of the server's certificates. */
        if (rc <= -0x2080 && rc >= -0x2F80)
            return rc == MBEDTLS_ERR_X509_ALLOC_FAILED ? AU_TLS_ERR_MEMORY : AU_TLS_ERR_CERT_MALFORMED;
        return AU_TLS_ERR_HANDSHAKE;
    }
}

static void fail(AuTls *t, int error)
{
    if (!t->failed) {
        t->failed = true;
        t->error = error;
    }
}

/* ---- Path building over the server's certificates and the pool ---- */

typedef struct PathCert {
    const uint8_t *der;
    size_t len;
    AuDerSpan iss, subj;
    bool from_pool;
} PathCert;

static bool span_eq(AuDerSpan a, AuDerSpan b) { return a.len == b.len && !memcmp(a.p, b.p, a.len); }

static bool is_anchor_dn(const AuTls *t, AuDerSpan dn) { return au_roots_has_subject(t->roots, dn.p, dn.len); }

static bool path_has(const PathCert *path, int n, const uint8_t *der, size_t len)
{
    for (int i = 0; i < n; i++)
        if (path[i].len == len && !memcmp(path[i].der, der, len))
            return true;
    return false;
}

/* The next certificate up from path[n-1]: one the server sent, else the
 * best pool certificate (one whose issuer is a root, else one that is not
 * self-issued, else any). False when there is none. */
static bool next_issuer(const AuTls *t, const PathCert *path, int n, PathCert *out)
{
    AuDerSpan want = path[n - 1].iss;
    for (int i = 1; i < t->nder; i++) {
        PathCert c;
        if (!t->der_len[i] || !au_x509_names(t->der[i], t->der_len[i], &c.iss, &c.subj))
            continue;
        if (span_eq(c.subj, want) && !path_has(path, n, t->der[i], t->der_len[i])) {
            c.der = t->der[i];
            c.len = t->der_len[i];
            c.from_pool = false;
            *out = c;
            return true;
        }
    }
    int best = -1, best_rank = 0;
    for (int i = 0; t->pool && i < t->pool->n; i++) {
        const AuPoolCert *pc = &t->pool->certs[i];
        AuDerSpan subj = {pc->der + pc->subj_off, pc->subj_len}, iss = {pc->der + pc->iss_off, pc->iss_len};
        if (!span_eq(subj, want) || path_has(path, n, pc->der, pc->len))
            continue;
        int rank = is_anchor_dn(t, iss) ? 3 : !span_eq(iss, subj) ? 2 : 1;
        if (rank > best_rank) {
            best = i;
            best_rank = rank;
        }
    }
    if (best < 0)
        return false;
    const AuPoolCert *pc = &t->pool->certs[best];
    out->der = pc->der;
    out->len = pc->len;
    out->iss.p = pc->der + pc->iss_off;
    out->iss.len = pc->iss_len;
    out->subj.p = pc->der + pc->subj_off;
    out->subj.len = pc->subj_len;
    out->from_pool = true;
    return true;
}

/* Called when the chain as sent did not reach a trust anchor. Builds a
 * path from the leaf and, when it differs from what the server sent, has
 * the library verify it (every check runs on the completed chain as if
 * the server had sent it). Returns the verification flags of the
 * completed chain, or orig when there is no other path or it is not
 * valid either. Records missing_issuer and aia_url when the path stops
 * for want of an issuer. */
static uint32_t complete_chain(AuTls *t, uint32_t orig)
{
    AuCertInfo *info = &t->info;
    if (t->nsent > AU_CERT_MAX_CHAIN || t->nder < 1 || !t->der_len[0])
        return orig;
    PathCert path[AU_CERT_MAX_CHAIN];
    int n = 0;
    path[0].der = t->der[0];
    path[0].len = t->der_len[0];
    path[0].from_pool = false;
    if (!au_x509_names(path[0].der, path[0].len, &path[0].iss, &path[0].subj))
        return orig;
    n = 1;
    bool missing = false;
    while (n < AU_CERT_MAX_CHAIN) {
        const PathCert *last = &path[n - 1];
        if (is_anchor_dn(t, last->iss) || span_eq(last->iss, last->subj))
            break; /* the library checks it against the root, or it is a root Aura lacks */
        if (!next_issuer(t, path, n, &path[n])) {
            missing = true;
            break;
        }
        n++;
    }
    if (missing) {
        info->missing_issuer = true;
        if (!au_x509_ca_issuers(path[n - 1].der, path[n - 1].len, info->aia_url, sizeof info->aia_url))
            info->aia_url[0] = 0;
    }
    /* The same certificates in the same order as sent: nothing new to try. */
    bool same = n == t->nder;
    for (int i = 0; same && i < n; i++)
        same = path[i].der == t->der[i];
    if (same)
        return orig;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    uint32_t flags = (uint32_t)-1;
    bool parsed = true;
    for (int i = 0; i < n && parsed; i++)
        parsed = mbedtls_x509_crt_parse_der(&chain, path[i].der, path[i].len) == 0;
    if (parsed) {
        au_tls_set_wall(t->now);
        mbedtls_x509_crt_verify_with_ca_cb(&chain, ca_cb, t, &mbedtls_x509_crt_profile_default, t->host, &flags, NULL,
                                           NULL);
    }
    mbedtls_x509_crt_free(&chain);
    if (flags != 0)
        return orig; /* report the chain as sent; missing_issuer says what to fetch */
    /* Valid: the viewer shows the path that was checked. */
    info->count = n;
    info->added = 0;
    info->missing_issuer = false;
    info->aia_url[0] = 0;
    for (int i = 0; i < n; i++) {
        au_cert_parse(path[i].der, path[i].len, &info->chain[i]);
        if (path[i].from_pool)
            info->added++;
    }
    return 0;
}

/* The handshake is complete. Decides whether the connection may be used. */
static void finish_handshake(AuTls *t)
{
    AuCertInfo *info = &t->info;
    uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    if (!t->saw_chain) {
        /* No certificates were verified. That is right only for a resumed
         * session, whose full handshake Aura checked (the stack stores a
         * session only after a valid, unexcepted chain). */
        if (!t->have_resume) {
            fail(t, AU_TLS_ERR_CERT_EMPTY_CHAIN);
            return;
        }
        t->resumed = true;
        info->count = 0;
        t->open = true;
        return;
    }
    info->count = t->nsent;
    for (int i = 0; i < t->nder && i < AU_CERT_MAX_CHAIN; i++) {
        AuCertEntry *e = &info->chain[i];
        memset(e, 0, sizeof *e);
        if (t->der_len[i])
            au_cert_parse(t->der[i], t->der_len[i], e);
        else
            snprintf(e->subject, sizeof e->subject, "(certificate too large to show)");
    }
    /* The usage flags concern the leaf's own extensions: they stand
     * whatever path is found. */
    uint32_t usage = flags == (uint32_t)-1 ? 0
                                           : flags & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE |
                                                      MBEDTLS_X509_BADCERT_NS_CERT_TYPE);
    if (cert_error(flags) == AU_TLS_ERR_CERT_NOT_TRUSTED)
        flags = complete_chain(t, flags) | usage;
    int err = cert_error(flags);
    info->error = err;
    if (err != 0) {
        /* A certificate error the user chose to continue past for this
         * exact leaf certificate (and only that one): the connection is
         * used and marked as overridden. */
        if (t->allow && t->der_len[0] && !memcmp(info->chain[0].sha256, t->allow_sha256, 32)) {
            info->overridden = true;
        } else {
            fail(t, err);
            return;
        }
    }
    t->open = true;
}

/* ---- Driving the library ---- */

/* Runs the library as far as the staging buffers allow: the handshake,
 * then pending plaintext out and decrypted data in. */
static void step(AuTls *t)
{
    if (t->failed || t->closed)
        return;
    au_tls_set_wall(t->now);
    if (!t->open) {
        int rc = mbedtls_ssl_handshake(&t->ssl);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;
        if (rc != 0) {
            /* A certificate the library could not accept even in optional
             * mode (unparseable, or a bad handshake signature) still has
             * its chain shown when it got that far. */
            if (t->saw_chain && t->info.count == 0) {
                t->info.count = t->nsent;
                for (int i = 0; i < t->nder; i++)
                    if (t->der_len[i])
                        au_cert_parse(t->der[i], t->der_len[i], &t->info.chain[i]);
            }
            fail(t, lib_error(rc));
            return;
        }
        finish_handshake(t);
        if (!t->open)
            return;
        /* TLS 1.2 has its session now; TLS 1.3 gets one with each ticket. */
        if (!t->resumed && mbedtls_ssl_get_version_number(&t->ssl) == MBEDTLS_SSL_VERSION_TLS1_2)
            t->new_session = true;
    }
    /* Plaintext the stack wrote. A write the library could not finish
     * must be repeated with the same bytes. */
    while (t->wr_len > 0 && (t->wr_flush || t->wr_pending)) {
        size_t n = t->wr_pending ? t->wr_pending : t->wr_len;
        int rc = mbedtls_ssl_write(&t->ssl, t->wr, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) {
            t->wr_pending = n;
            break;
        }
        if (rc <= 0) {
            fail(t, lib_error(rc));
            return;
        }
        memmove(t->wr, t->wr + rc, t->wr_len - (size_t)rc);
        t->wr_len -= (size_t)rc;
        t->wr_pending = 0;
    }
    if (t->wr_len == 0) {
        t->wr_flush = false;
        if (t->wr) {
            memset(t->wr, 0, WRITE_CAP);
            au_free(t->wr);
            t->wr = NULL;
        }
    }
    /* Decrypted data for the stack. */
    while (t->app_len == t->app_pos) {
        t->app_len = t->app_pos = 0;
        int rc = mbedtls_ssl_read(&t->ssl, t->app, APP_CAP);
        if (rc > 0) {
            t->app_len = (size_t)rc;
            break;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            t->new_session = true;
            continue;
        }
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF) {
            t->closed = true;
            break;
        }
        fail(t, lib_error(rc));
        return;
    }
}

/* ---- Setup ---- */

static int g_max_version; /* au_tls_limit_version */

void au_tls_limit_version(int version) { g_max_version = version; }

AuTls *au_tls_new(const AuRootStore *roots, const char *host, int64_t now, const uint8_t seed[32],
                  const AuTlsSession *resume)
{
    if (!roots)
        return NULL;
    AuTls *t = au_calloc(sizeof *t);
    t->roots = roots;
    t->now = now;
    snprintf(t->host, sizeof t->host, "%s", host);
    snprintf(t->info.host, sizeof t->info.host, "%s", host);
    t->info.checked_at = now;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ssl_session_init(&t->resume);
    /* The seed goes into the pool the library's generator reseeds from;
     * the first one starts the library. */
    if (!au_tls_entropy_feed(seed, 32)) {
        fail(t, AU_TLS_ERR_NO_RANDOM);
        return t;
    }
    au_tls_set_wall(now);
    t->in = au_try_alloc(IN_CAP);
    t->out = au_try_alloc(OUT_CAP);
    t->app = au_try_alloc(APP_CAP);
    if (!t->in || !t->out || !t->app) {
        fail(t, AU_TLS_ERR_MEMORY);
        return t;
    }
    mbedtls_ssl_config *conf = &t->conf;
    int rc = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        fail(t, lib_error(rc));
        return t;
    }
    mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(conf, g_max_version == 0x0303 ? MBEDTLS_SSL_VERSION_TLS1_2
                                                                   : MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_ciphersuites(conf, suites);
    mbedtls_ssl_conf_groups(conf, groups);
    mbedtls_ssl_conf_sig_algs(conf, sig_algs);
    /* Optional: finish_handshake reads the verdict and decides. */
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_cb(conf, ca_cb, t);
    mbedtls_ssl_conf_session_tickets(conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    t->alpn[0] = "http/1.1";
    t->alpn[1] = NULL;
    if ((rc = mbedtls_ssl_conf_alpn_protocols(conf, t->alpn)) != 0 || (rc = mbedtls_ssl_setup(&t->ssl, conf)) != 0 ||
        (rc = mbedtls_ssl_set_hostname(&t->ssl, host)) != 0) {
        fail(t, lib_error(rc));
        return t;
    }
    mbedtls_ssl_set_verify(&t->ssl, vrfy_cb, t);
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    /* A session that does not load or set (another library version, an
     * expired ticket) is dropped: the handshake is then a full one. */
    if (resume && resume->valid && resume->len > 0 && resume->len <= sizeof resume->data &&
        mbedtls_ssl_session_load(&t->resume, resume->data, resume->len) == 0 &&
        mbedtls_ssl_set_session(&t->ssl, &t->resume) == 0)
        t->have_resume = true;
    return t;
}

void au_tls_allow_cert(AuTls *t, const uint8_t sha256[32])
{
    t->allow = true;
    memcpy(t->allow_sha256, sha256, 32);
}

void au_tls_set_pool(AuTls *t, const AuCertPool *pool) { t->pool = pool; }

void au_tls_allow_legacy_kex(AuTls *t)
{
    /* The list is read when the ClientHello is written, at the first step. */
    if (!t->failed)
        mbedtls_ssl_conf_ciphersuites(&t->conf, suites_legacy);
}

bool au_tls_cipher_is_legacy(const char *cipher) { return cipher && !strncmp(cipher, "RSA-", 4); }

void au_tls_free(AuTls *t)
{
    if (!t)
        return;
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ssl_session_free(&t->resume);
    drop_chain(t);
    if (t->app)
        memset(t->app, 0, APP_CAP);
    if (t->wr)
        memset(t->wr, 0, WRITE_CAP);
    au_free(t->in);
    au_free(t->out);
    au_free(t->app);
    au_free(t->wr);
    memset(t, 0, sizeof *t);
    au_free(t);
}

AuTlsState au_tls_state(AuTls *t)
{
    step(t);
    if (t->failed)
        return AU_TLS_FAILED;
    if (t->closed && t->app_len == t->app_pos)
        return AU_TLS_CLOSED;
    return t->open ? AU_TLS_OPEN : AU_TLS_HANDSHAKE;
}

const uint8_t *au_tls_out(AuTls *t, size_t *len)
{
    *len = 0;
    if (t->failed)
        return NULL;
    if (t->out_pos == t->out_len)
        step(t);
    if (t->failed || t->out_pos == t->out_len)
        return NULL;
    *len = t->out_len - t->out_pos;
    return t->out + t->out_pos;
}

void au_tls_out_done(AuTls *t, size_t n)
{
    t->out_pos += n;
    if (t->out_pos >= t->out_len)
        t->out_pos = t->out_len = 0;
    step(t);
}

uint8_t *au_tls_in(AuTls *t, size_t *cap)
{
    *cap = 0;
    if (t->failed || t->closed || t->eof)
        return NULL;
    if (t->in_pos > 0) {
        memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (t->in_len == IN_CAP) {
        step(t); /* the library may take some now */
        if (t->failed || t->in_len == IN_CAP)
            return NULL;
        if (t->in_pos > 0) {
            memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
            t->in_len -= t->in_pos;
            t->in_pos = 0;
        }
    }
    *cap = IN_CAP - t->in_len;
    return t->in + t->in_len;
}

void au_tls_in_done(AuTls *t, size_t n)
{
    /* Whether the peer speaks TLS at all: its first five bytes are a
     * record header (handshake or alert, version 3.x, a possible length). */
    for (size_t i = 0; i < n && t->nfirst < sizeof t->first; i++)
        t->first[t->nfirst++] = t->in[t->in_len + i];
    if (t->nfirst == sizeof t->first && !t->spoke_tls) {
        const uint8_t *f = t->first;
        size_t rl = ((size_t)f[3] << 8) | f[4];
        t->spoke_tls = (f[0] == 22 || f[0] == 21) && f[1] == 3 && f[2] <= 4 && rl > 0 && rl <= 16384 + 2048;
    }
    t->in_len += n;
    step(t);
}

const uint8_t *au_tls_read(AuTls *t, size_t *len)
{
    *len = 0;
    if (t->failed)
        return NULL;
    if (t->app_pos == t->app_len)
        step(t);
    if (t->failed || t->app_pos == t->app_len)
        return NULL;
    *len = t->app_len - t->app_pos;
    return t->app + t->app_pos;
}

void au_tls_read_done(AuTls *t, size_t n)
{
    t->app_pos += n;
    if (t->app_pos >= t->app_len)
        t->app_pos = t->app_len = 0;
}

uint8_t *au_tls_write(AuTls *t, size_t *cap)
{
    *cap = 0;
    if (t->failed || t->closed || !t->open)
        return NULL;
    if (t->wr_len == WRITE_CAP || t->wr_pending)
        step(t);
    if (t->failed || t->wr_pending || t->wr_len == WRITE_CAP)
        return NULL;
    if (!t->wr && !(t->wr = au_try_alloc(WRITE_CAP))) {
        fail(t, AU_TLS_ERR_MEMORY);
        return NULL;
    }
    *cap = WRITE_CAP - t->wr_len;
    return t->wr + t->wr_len;
}

void au_tls_write_done(AuTls *t, size_t n)
{
    if (!t->wr)
        return;
    t->wr_len += n;
    if (t->wr_len == WRITE_CAP) {
        t->wr_flush = true; /* a full buffer goes out as one record */
        step(t);
    }
}

void au_tls_flush(AuTls *t)
{
    if (t->wr_len > 0) {
        t->wr_flush = true;
        step(t);
    }
}

void au_tls_close(AuTls *t)
{
    /* TCP ended. If the handshake had not finished, that is a failure;
     * afterwards the library reports the end once it has read what is
     * staged. */
    t->eof = true;
    step(t);
    if (!t->open && !t->failed)
        fail(t, AU_TLS_ERR_IO);
}

int au_tls_error(AuTls *t) { return t->error; }

bool au_tls_peer_spoke_tls(AuTls *t) { return t->spoke_tls || t->info.count > 0; }

static bool is_cert_error(int e) { return e >= AU_TLS_ERR_CERT_EXPIRED && e <= AU_TLS_ERR_CERT_MALFORMED; }

AuNetError au_tls_net_error(AuTls *t)
{
    return is_cert_error(t->error) || is_cert_error(t->info.error) ? AU_NET_ERR_CERT : AU_NET_ERR_TLS;
}

const char *au_tls_error_text(int e)
{
    switch (e) {
    case 0: return "no error";
    case AU_TLS_ERR_CERT_EXPIRED: return "the certificate has expired or is not yet valid";
    case AU_TLS_ERR_CERT_NOT_TRUSTED: return "the certificate is not signed by a trusted authority";
    case AU_TLS_ERR_CERT_BAD_NAME: return "the certificate is for a different site";
    case AU_TLS_ERR_CERT_KEY_USAGE: return "the certificate's key may not be used for this";
    case AU_TLS_ERR_CERT_WEAK_KEY: return "the certificate's key is too weak";
    case AU_TLS_ERR_CERT_UNSUPPORTED: return "the certificate uses an unsupported algorithm";
    case AU_TLS_ERR_CERT_EMPTY_CHAIN: return "the server sent no certificate";
    case AU_TLS_ERR_CERT_MALFORMED: return "the certificate is malformed";
    case AU_TLS_ERR_VERSION: return "the server does not support TLS 1.2 or TLS 1.3";
    case AU_TLS_ERR_ALERT: return "the server sent a fatal alert (often: no common cipher or version)";
    case AU_TLS_ERR_IO: return "the connection closed during the handshake";
    case AU_TLS_ERR_BAD_MAC: return "a record failed its integrity check";
    case AU_TLS_ERR_NO_RANDOM: return "no random seed";
    case AU_TLS_ERR_MEMORY: return "not enough memory for the secure connection";
    case AU_TLS_ERR_ILLEGAL: return "the server's handshake was not valid";
    default: return "the TLS handshake failed";
    }
}

const char *au_tls_version_name(int version)
{
    return version == 0x0304 ? "TLS 1.3" : version == 0x0303 ? "TLS 1.2" : "TLS";
}

const AuCertInfo *au_tls_cert_info(AuTls *t)
{
    if (t->info.error == 0 && t->failed)
        t->info.error = t->error;
    t->info.reason = au_tls_error_text(t->info.error ? t->info.error : (t->failed ? t->error : 0));
    return &t->info;
}

static const char *suite_name(int s)
{
    switch (s) {
    case MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256: return "TLS_CHACHA20_POLY1305_SHA256";
    case MBEDTLS_TLS1_3_AES_128_GCM_SHA256: return "TLS_AES_128_GCM_SHA256";
    case MBEDTLS_TLS1_3_AES_256_GCM_SHA384: return "TLS_AES_256_GCM_SHA384";
    case MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-RSA-CHACHA20-POLY1305";
    case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: return "ECDHE-RSA-AES128-GCM-SHA256";
    case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: return "ECDHE-RSA-AES256-GCM-SHA384";
    case MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256: return "RSA-AES128-GCM-SHA256";
    case MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384: return "RSA-AES256-GCM-SHA384";
    default: return "unknown";
    }
}

void au_tls_security(AuTls *t, AuSecurityInfo *si)
{
    memset(si, 0, sizeof *si);
    si->tls = true;
    if (!t->open) {
        si->cipher = "unknown";
        si->alpn = "";
        return;
    }
    si->version = (int)mbedtls_ssl_get_version_number(&t->ssl);
    si->cipher = suite_name(mbedtls_ssl_get_ciphersuite_id_from_ssl(&t->ssl));
    const char *p = mbedtls_ssl_get_alpn_protocol(&t->ssl);
    si->alpn = p ? p : "";
    si->resumed = t->resumed;
    si->legacy_kex = au_tls_cipher_is_legacy(si->cipher);
}

bool au_tls_save_session(AuTls *t, AuTlsSession *out)
{
    out->valid = false;
    out->len = 0;
    if (!t->open || !t->new_session)
        return false;
    t->new_session = false;
    mbedtls_ssl_session s;
    mbedtls_ssl_session_init(&s);
    size_t n = 0;
    bool ok = mbedtls_ssl_get_session(&t->ssl, &s) == 0 &&
              mbedtls_ssl_session_save(&s, out->data, sizeof out->data, &n) == 0 && n > 0;
    mbedtls_ssl_session_free(&s);
    if (!ok) {
        memset(out->data, 0, sizeof out->data); /* a session too large to keep, or none to give */
        return false;
    }
    out->len = (uint16_t)n;
    out->valid = true;
    return true;
}
