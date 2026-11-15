/* TLS client over BearSSL, sans I/O: the caller moves ciphertext between
 * the engine and the socket, and plaintext between the engine and HTTP.
 *
 * Configuration (fixed): TLS 1.2 only; ECDHE key exchange with ECDSA or
 * RSA certificates; ChaCha20-Poly1305 first (fastest without AES
 * hardware), then AES-128-GCM and AES-256-GCM; curves X25519, P-256,
 * P-384; SNI; ALPN "http/1.1"; secure renegotiation refused. Only the
 * 32-bit BearSSL implementations are used (i31/m31, aes_ct,
 * ghash_ctmul32, chacha20_ct, poly1305_ctmul), so the host runs the same
 * code as the PowerPC build.
 *
 * Certificates are checked against the root store (roots.h) with
 * BearSSL's X.509 minimal engine: chain to a trust anchor, validity at
 * the time given, key usage, and the host name (SNI name). The chain the
 * server sent is recorded for the certificate viewer (AuCertInfo). */
#ifndef AURA_NET_TLS_H
#define AURA_NET_TLS_H

#include "net.h"

#define AU_CERT_MAX_CHAIN 8

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
    int count;           /* certificates the server sent (at most AU_CERT_MAX_CHAIN kept) */
    AuCertEntry chain[AU_CERT_MAX_CHAIN];
    int error;           /* BearSSL error code, 0 when valid */
    const char *reason;  /* English text for the error page */
    bool clock_wrong;    /* the system date is before the root store's date */
    bool hsts;           /* the host is an HSTS host: the error cannot be overridden */
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
