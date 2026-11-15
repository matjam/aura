/* TLS client glue over BearSSL. See tls.h. */
#include "tls.h"

#include "roots.h"

#include "bearssl.h"

#include <stdio.h>
#include <string.h>

#define CERT_KEEP_MAX 16384 /* bytes of each chain certificate kept for the viewer */

/* An X.509 engine that records the chain and forwards to the minimal
 * engine. */
typedef struct ChainRecorder {
    const br_x509_class *vtable;
    br_x509_minimal_context minimal;
    struct AuTls *owner;
    uint8_t *cur;
    size_t cur_len, cur_cap;
    bool cur_overflow;
} ChainRecorder;

struct AuTls {
    br_ssl_client_context cc;
    ChainRecorder xc;
    uint8_t iobuf[BR_SSL_BUFSIZE_BIDI];
    AuCertInfo info;
    bool failed;
    int error;
    bool closed;
    const char *alpn_names[1];
};

static const uint16_t suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};

/* ---- The recording X.509 wrapper ---- */

static ChainRecorder *rec_of(const br_x509_class **ctx)
{
    return (ChainRecorder *)(void *)ctx;
}

static void rec_start_chain(const br_x509_class **ctx, const char *server_name)
{
    ChainRecorder *r = rec_of(ctx);
    r->owner->info.count = 0;
    r->minimal.vtable->start_chain(&r->minimal.vtable, server_name);
}

static void rec_start_cert(const br_x509_class **ctx, uint32_t length)
{
    ChainRecorder *r = rec_of(ctx);
    r->cur_len = 0;
    r->cur_overflow = length > CERT_KEEP_MAX;
    r->minimal.vtable->start_cert(&r->minimal.vtable, length);
}

static void rec_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    ChainRecorder *r = rec_of(ctx);
    if (!r->cur_overflow) {
        if (r->cur_len + len > CERT_KEEP_MAX) {
            r->cur_overflow = true;
        } else {
            if (r->cur_len + len > r->cur_cap) {
                size_t nc = r->cur_cap ? r->cur_cap : 2048;
                while (nc < r->cur_len + len)
                    nc *= 2;
                r->cur = au_realloc(r->cur, nc);
                r->cur_cap = nc;
            }
            memcpy(r->cur + r->cur_len, buf, len);
            r->cur_len += len;
        }
    }
    r->minimal.vtable->append(&r->minimal.vtable, buf, len);
}

static void rec_end_cert(const br_x509_class **ctx)
{
    ChainRecorder *r = rec_of(ctx);
    AuCertInfo *info = &r->owner->info;
    if (info->count < AU_CERT_MAX_CHAIN) {
        AuCertEntry *e = &info->chain[info->count];
        memset(e, 0, sizeof *e);
        if (!r->cur_overflow)
            au_cert_parse(r->cur, r->cur_len, e);
        else
            snprintf(e->subject, sizeof e->subject, "(certificate too large to show)");
    }
    info->count++;
    r->minimal.vtable->end_cert(&r->minimal.vtable);
}

static unsigned rec_end_chain(const br_x509_class **ctx)
{
    ChainRecorder *r = rec_of(ctx);
    unsigned err = r->minimal.vtable->end_chain(&r->minimal.vtable);
    r->owner->info.error = (int)err;
    return err;
}

static const br_x509_pkey *rec_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const ChainRecorder *r = (const ChainRecorder *)(const void *)ctx;
    return r->minimal.vtable->get_pkey(&r->minimal.vtable, usages);
}

static const br_x509_class rec_vtable = {
    sizeof(ChainRecorder), rec_start_chain, rec_start_cert, rec_append, rec_end_cert, rec_end_chain, rec_get_pkey,
};

/* ---- Setup ---- */

AuTls *au_tls_new(const AuRootStore *roots, const char *host, int64_t now, const uint8_t seed[32],
                  const AuTlsSession *resume)
{
    if (!roots)
        return NULL;
    AuTls *t = au_calloc(sizeof *t);
    br_ssl_client_context *cc = &t->cc;
    br_ssl_engine_context *eng = &cc->eng;
    br_ssl_client_zero(cc);
    br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(eng, suites, sizeof suites / sizeof suites[0]);
    br_ssl_engine_set_all_flags(eng, BR_OPT_NO_RENEGOTIATION);
    br_ssl_client_set_rsapub(cc, br_rsa_i31_public);
    br_ssl_engine_set_rsavrfy(eng, br_rsa_i31_pkcs1_vrfy);
    br_ssl_engine_set_ec(eng, &br_ec_all_m31);
    br_ssl_engine_set_ecdsa(eng, br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_hash(eng, br_sha1_ID, &br_sha1_vtable);
    br_ssl_engine_set_hash(eng, br_sha224_ID, &br_sha224_vtable);
    br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(eng, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(eng, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
    br_ssl_engine_set_aes_ctr(eng, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(eng, &br_ghash_ctmul32);
    br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_chacha20(eng, &br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(eng, &br_poly1305_ctmul_run);
    br_ssl_engine_set_chapol(eng, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);

    size_t nanchors;
    const br_x509_trust_anchor *anchors = au_roots_anchors(roots, &nanchors);
    br_x509_minimal_context *xm = &t->xc.minimal;
    br_x509_minimal_init(xm, &br_sha256_vtable, anchors, nanchors);
    br_x509_minimal_set_hash(xm, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(xm, br_sha224_ID, &br_sha224_vtable);
    br_x509_minimal_set_hash(xm, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(xm, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xm, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_rsa(xm, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(xm, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    /* BearSSL counts days from 0000-01-01 (proleptic Gregorian); 1970-01-01
     * is day 719528. */
    int64_t days = (now >= 0 ? now / 86400 : (now - 86399) / 86400);
    int64_t secs = now - days * 86400;
    br_x509_minimal_set_time(xm, (uint32_t)(days + 719528), (uint32_t)secs);
    t->xc.vtable = &rec_vtable;
    t->xc.owner = t;
    br_ssl_engine_set_x509(eng, &t->xc.vtable);

    br_ssl_engine_set_buffer(eng, t->iobuf, sizeof t->iobuf, 1);
    t->alpn_names[0] = "http/1.1";
    br_ssl_engine_set_protocol_names(eng, t->alpn_names, 1);

    snprintf(t->info.host, sizeof t->info.host, "%s", host);
    t->info.checked_at = now;
    if (resume && resume->valid) {
        br_ssl_session_parameters sp;
        memcpy(&sp, resume->data, sizeof sp < sizeof resume->data ? sizeof sp : sizeof resume->data);
        br_ssl_engine_set_session_parameters(eng, &sp);
    }
    if (!br_ssl_client_reset(cc, host, resume && resume->valid)) {
        t->failed = true;
        t->error = br_ssl_engine_last_error(eng);
        return t;
    }
    br_ssl_engine_inject_entropy(eng, seed, 32);
    return t;
}

void au_tls_free(AuTls *t)
{
    if (!t)
        return;
    au_free(t->xc.cur);
    memset(t, 0, sizeof *t);
    au_free(t);
}

static void check_fail(AuTls *t)
{
    if (t->failed)
        return;
    unsigned st = br_ssl_engine_current_state(&t->cc.eng);
    if (st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&t->cc.eng);
        if (err != BR_ERR_OK) {
            t->failed = true;
            t->error = err;
        } else {
            t->closed = true;
        }
    }
}

AuTlsState au_tls_state(AuTls *t)
{
    check_fail(t);
    if (t->failed)
        return AU_TLS_FAILED;
    if (t->closed)
        return AU_TLS_CLOSED;
    unsigned st = br_ssl_engine_current_state(&t->cc.eng);
    return (st & BR_SSL_SENDAPP) ? AU_TLS_OPEN : AU_TLS_HANDSHAKE;
}

const uint8_t *au_tls_out(AuTls *t, size_t *len)
{
    *len = 0;
    if (t->failed)
        return NULL;
    if (!(br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDREC))
        return NULL;
    return br_ssl_engine_sendrec_buf(&t->cc.eng, len);
}

void au_tls_out_done(AuTls *t, size_t n)
{
    br_ssl_engine_sendrec_ack(&t->cc.eng, n);
    check_fail(t);
}

uint8_t *au_tls_in(AuTls *t, size_t *cap)
{
    *cap = 0;
    if (t->failed)
        return NULL;
    if (!(br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_RECVREC))
        return NULL;
    return br_ssl_engine_recvrec_buf(&t->cc.eng, cap);
}

void au_tls_in_done(AuTls *t, size_t n)
{
    br_ssl_engine_recvrec_ack(&t->cc.eng, n);
    check_fail(t);
}

const uint8_t *au_tls_read(AuTls *t, size_t *len)
{
    *len = 0;
    if (t->failed)
        return NULL;
    if (!(br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_RECVAPP))
        return NULL;
    return br_ssl_engine_recvapp_buf(&t->cc.eng, len);
}

void au_tls_read_done(AuTls *t, size_t n)
{
    br_ssl_engine_recvapp_ack(&t->cc.eng, n);
}

uint8_t *au_tls_write(AuTls *t, size_t *cap)
{
    *cap = 0;
    if (t->failed)
        return NULL;
    if (!(br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDAPP))
        return NULL;
    return br_ssl_engine_sendapp_buf(&t->cc.eng, cap);
}

void au_tls_write_done(AuTls *t, size_t n)
{
    br_ssl_engine_sendapp_ack(&t->cc.eng, n);
}

void au_tls_flush(AuTls *t)
{
    br_ssl_engine_flush(&t->cc.eng, 0);
}

void au_tls_close(AuTls *t)
{
    /* TCP ended. If the handshake had not finished, that is a failure. */
    if (!(br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDAPP) && !t->failed) {
        t->failed = true;
        t->error = BR_ERR_IO;
    }
}

int au_tls_error(AuTls *t) { return t->error; }

AuNetError au_tls_net_error(AuTls *t)
{
    int e = t->error;
    if ((e >= 32 && e < 64) || (t->info.error >= 32 && t->info.error < 64 && t->info.error != BR_ERR_X509_OK))
        return AU_NET_ERR_CERT;
    return AU_NET_ERR_TLS;
}

const char *au_tls_error_text(int e)
{
    switch (e) {
    case 0: return "no error";
    case BR_ERR_X509_EXPIRED: return "the certificate has expired or is not yet valid";
    case BR_ERR_X509_NOT_TRUSTED: return "the certificate is not signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for a different site";
    case BR_ERR_X509_NOT_CA: return "an intermediate certificate is not allowed to sign certificates";
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "the certificate's key may not be used for this";
    case BR_ERR_X509_WEAK_PUBLIC_KEY: return "the certificate's key is too weak";
    case BR_ERR_X509_BAD_SIGNATURE: return "a certificate signature is invalid";
    case BR_ERR_X509_CRITICAL_EXTENSION: return "the certificate has an unsupported critical extension";
    case BR_ERR_X509_UNSUPPORTED: return "the certificate uses an unsupported algorithm";
    case BR_ERR_X509_EMPTY_CHAIN: return "the server sent no certificate";
    case BR_ERR_X509_TIME_UNKNOWN: return "the time is unknown";
    case BR_ERR_UNSUPPORTED_VERSION:
    case BR_ERR_BAD_VERSION: return "the server does not support TLS 1.2";
    case BR_ERR_BAD_CIPHER_SUITE: return "the server chose a cipher Aura does not support";
    case BR_ERR_IO: return "the connection closed during the handshake";
    case BR_ERR_BAD_MAC: return "a record failed its integrity check";
    case BR_ERR_NO_RANDOM: return "no random seed";
    default:
        if (e >= BR_ERR_SEND_FATAL_ALERT)
            return "Aura sent a fatal alert";
        if (e >= BR_ERR_RECV_FATAL_ALERT)
            return "the server sent a fatal alert (often: no common cipher or version)";
        if (e >= 32 && e < 64)
            return "the certificate is malformed";
        return "the TLS handshake failed";
    }
}

const AuCertInfo *au_tls_cert_info(AuTls *t)
{
    if (t->info.error == 0 && t->failed)
        t->info.error = t->error;
    t->info.reason = au_tls_error_text(t->info.error && t->info.error != BR_ERR_X509_OK ? t->info.error
                                                                                        : (t->failed ? t->error : 0));
    return &t->info;
}

static const char *suite_name(uint16_t s)
{
    switch (s) {
    case BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-RSA-CHACHA20-POLY1305";
    case BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: return "ECDHE-RSA-AES128-GCM-SHA256";
    case BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: return "ECDHE-RSA-AES256-GCM-SHA384";
    default: return "unknown";
    }
}

void au_tls_security(AuTls *t, AuSecurityInfo *si)
{
    memset(si, 0, sizeof *si);
    si->tls = true;
    si->version = br_ssl_engine_get_version(&t->cc.eng);
    si->cipher = suite_name(t->cc.eng.session.cipher_suite);
    const char *p = br_ssl_engine_get_selected_protocol(&t->cc.eng);
    si->alpn = p ? p : "";
    si->resumed = br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDAPP ? t->info.count == 0 : false;
}

bool au_tls_save_session(AuTls *t, AuTlsSession *out)
{
    br_ssl_session_parameters sp;
    br_ssl_engine_get_session_parameters(&t->cc.eng, &sp);
    if (sizeof sp > sizeof out->data || sp.session_id_len == 0) {
        out->valid = false;
        return false;
    }
    memcpy(out->data, &sp, sizeof sp);
    out->valid = true;
    memset(&sp, 0, sizeof sp);
    return true;
}
