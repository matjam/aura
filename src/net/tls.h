/* TLS client over BearSSL, sans I/O: the caller moves ciphertext between
 * the engine and the socket, and plaintext between the engine and HTTP.
 *
 * Configuration (fixed): TLS 1.2 only; ECDHE key exchange with ECDSA or
 * RSA certificates; ChaCha20-Poly1305 first (fastest without AES
 * hardware), then AES-128-GCM and AES-256-GCM; RSA key exchange with
 * AES-GCM last, for servers without ECDHE; curves X25519, P-256,
 * P-384; SNI; ALPN "http/1.1"; secure renegotiation refused. Only the
 * 32-bit BearSSL implementations are used (i31/m31, aes_ct,
 * ghash_ctmul32, chacha20_ct, poly1305_ctmul), so the host runs the same
 * code as the PowerPC build.
 *
 * Certificates are checked against the root store (roots.h) with
 * BearSSL's X.509 minimal engine: chain to a trust anchor, validity at
 * the time given, key usage, and the host name (SNI name). The chain the
 * server sent is recorded for the certificate viewer (AuCertInfo).
 *
 * Incomplete chains. Some servers send their certificate without the
 * intermediate that links it to a root. When the chain as sent does not
 * reach a trust anchor, the X.509 wrapper builds a path from the leaf:
 * each next certificate is one the server sent, else one from the
 * intermediates pool (AuCertPool, filled from caIssuers URLs), whose
 * subject is the previous certificate's issuer. The path is then replayed
 * into the same minimal engine, from start_chain, exactly as if the
 * server had sent it: every check (signatures, dates, CA flags, key
 * usage, the host name, and the trust anchor at the end) runs again on
 * the completed chain. Pool certificates are never trust anchors; a path
 * that does not end at a bundled root fails as before. When the path
 * stops for want of an issuer, AuCertInfo names the last certificate's
 * caIssuers URL (aia_url), and the HTTP layer may fetch it into the pool
 * and connect again. */
#ifndef AURA_NET_TLS_H
#define AURA_NET_TLS_H

#include "net.h"

#define AU_CERT_MAX_CHAIN 8

/* ---- The intermediates pool ----
 * Certificates fetched from caIssuers URLs (untrusted input that only
 * helps build a path to a root). At most AU_POOL_MAX certificates of at
 * most AU_POOL_CERT_MAX bytes; the oldest goes first. */
#define AU_POOL_MAX 32
#define AU_POOL_CERT_MAX 8192

typedef struct AuPoolCert {
    uint8_t *der;
    size_t len;
    size_t iss_off, iss_len;   /* the issuer Name (TLV) within der */
    size_t subj_off, subj_len; /* the subject Name (TLV) within der */
} AuPoolCert;

typedef struct AuCertPool {
    AuPoolCert certs[AU_POOL_MAX];
    int n;
    bool dirty; /* changed since au_cert_pool_clear_dirty */
} AuCertPool;

void au_cert_pool_init(AuCertPool *p);
void au_cert_pool_free(AuCertPool *p);
/* Adds a copy of a DER certificate. Returns true when it was new; false
 * when it is malformed, too large, or already there. */
bool au_cert_pool_add(AuCertPool *p, const uint8_t *der, size_t len);
/* File form: "AURI", u8 version 1, u8 0, u16 count, then count times
 * u16 length and the DER bytes. Parsing replaces the pool's contents and
 * skips certificates that do not parse; false when the file is not one. */
void au_cert_pool_serialize(const AuCertPool *p, AuBuf *out);
bool au_cert_pool_parse(AuCertPool *p, const void *data, size_t len);

/* ---- DER helpers for path building (certinfo.c) ---- */
typedef struct AuDerSpan {
    const uint8_t *p;
    size_t len;
} AuDerSpan;

/* The issuer and subject Names of a certificate, as whole TLVs (the bytes
 * BearSSL compares). False when the certificate is malformed or has
 * trailing bytes. */
bool au_x509_names(const uint8_t *der, size_t len, AuDerSpan *issuer, AuDerSpan *subject);
/* The first http: caIssuers URL of the Authority Information Access
 * extension, NUL-terminated in url (at most cap - 1 printable ASCII
 * characters). False when there is none, it is not http:, or too long. */
bool au_x509_ca_issuers(const uint8_t *der, size_t len, char *url, size_t cap);
/* Certificates in a caIssuers response: a DER certificate, a PKCS#7
 * SignedData (DER, "certs-only") or PEM (CERTIFICATE or PKCS7 blocks).
 * Calls sink with each certificate's DER (at most max), and returns how
 * many were found. Every length is bounded by len. */
int au_cert_bundle_parse(const uint8_t *data, size_t len, int max,
                         void (*sink)(void *ctx, const uint8_t *der, size_t len), void *ctx);

typedef struct AuCertEntry {
    char subject[192];   /* "CN=example.com, O=Example, C=US" */
    char issuer[192];
    int64_t not_before;  /* seconds since 1970 UTC; 0 when unknown */
    int64_t not_after;
    uint8_t sha256[32];  /* fingerprint of the DER certificate */
    char san[256];       /* DNS names from subjectAltName, comma-separated */
} AuCertEntry;

struct AuCertInfo {
    char host[256];      /* the name that was checked */
    int count;           /* certificates in the chain (at most AU_CERT_MAX_CHAIN kept) */
    AuCertEntry chain[AU_CERT_MAX_CHAIN];
    int added;           /* of count, the last ones came from the intermediates pool, not the server */
    bool missing_issuer; /* the chain stops at a certificate whose issuer Aura does not have */
    char aia_url[256];   /* with missing_issuer: that certificate's caIssuers URL, or "" */
    int error;           /* BearSSL error code, 0 when valid */
    const char *reason;  /* English text for the error page */
    bool clock_wrong;    /* the system date is before the root store's date */
    bool hsts;           /* the host is an HSTS host: the error cannot be overridden */
    bool overridden;     /* error set, but the user's session exception let the handshake go on */
    int64_t checked_at;  /* the time used for validation */
};

typedef enum { AU_TLS_HANDSHAKE, AU_TLS_OPEN, AU_TLS_CLOSED, AU_TLS_FAILED } AuTlsState;

typedef struct AuTls AuTls;

/* Saved session parameters for resumption (opaque, about 100 bytes). */
typedef struct AuTlsSession {
    uint8_t data[128];
    bool valid;
} AuTlsSession;

/* Starts a client handshake to host (the SNI name and the name checked
 * against the certificate) with certificates validated at time now
 * (seconds since 1970 UTC). seed is fresh random input (32 bytes) for the
 * engine's DRBG. resume may be NULL. Returns NULL only when roots is NULL. */
AuTls *au_tls_new(const AuRootStore *roots, const char *host, int64_t now, const uint8_t seed[32],
                  const AuTlsSession *resume);
void au_tls_free(AuTls *t);
AuTlsState au_tls_state(AuTls *t);
/* The user's exception for this connection: a certificate error (expired,
 * wrong host, untrusted issuer...) on a chain whose leaf certificate has
 * this SHA-256 fingerprint does not stop the handshake; AuCertInfo keeps
 * the error and sets overridden. Call before the server's certificates
 * arrive (right after au_tls_new). The stack never calls it for an HSTS
 * host. */
void au_tls_allow_cert(AuTls *t, const uint8_t sha256[32]);
/* Intermediates that may complete a chain the server sent short (read
 * when the server's certificates arrive; the pool must outlive t). */
void au_tls_set_pool(AuTls *t, const AuCertPool *pool);

/* Ciphertext to send: a pointer and length (0 when none); then report how
 * much the socket took. */
const uint8_t *au_tls_out(AuTls *t, size_t *len);
void au_tls_out_done(AuTls *t, size_t n);
/* Room for received ciphertext (0 when the engine wants none now). */
uint8_t *au_tls_in(AuTls *t, size_t *cap);
void au_tls_in_done(AuTls *t, size_t n);
/* Decrypted application data available, and consuming it. */
const uint8_t *au_tls_read(AuTls *t, size_t *len);
void au_tls_read_done(AuTls *t, size_t n);
/* Room for application data to encrypt; then au_tls_write_done and,
 * when a message is complete, au_tls_flush. */
uint8_t *au_tls_write(AuTls *t, size_t *cap);
void au_tls_write_done(AuTls *t, size_t n);
void au_tls_flush(AuTls *t);
/* The peer closed (TCP end without close_notify is reported by the
 * caller, which treats it as truncation only where framing needs it). */
void au_tls_close(AuTls *t);

/* After AU_TLS_FAILED: the BearSSL error and the stack's error class
 * (AU_NET_ERR_CERT for certificate problems, AU_NET_ERR_TLS otherwise). */
int au_tls_error(AuTls *t);
AuNetError au_tls_net_error(AuTls *t);
const char *au_tls_error_text(int bearssl_error);
const AuCertInfo *au_tls_cert_info(AuTls *t);
void au_tls_security(AuTls *t, AuSecurityInfo *si);
/* After the handshake: the session for later resumption. */
bool au_tls_save_session(AuTls *t, AuTlsSession *out);

/* Parses one DER certificate into an AuCertEntry (subject, issuer,
 * validity, SAN DNS names, fingerprint). Returns false when the DER is
 * malformed; the entry then holds what was read. Bounded; never reads
 * past len. */
bool au_cert_parse(const uint8_t *der, size_t len, AuCertEntry *e);

#endif
