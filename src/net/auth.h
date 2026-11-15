/* HTTP authentication (Basic, RFC 7617; Digest, RFC 7616 with MD5 and
 * SHA-256, qop=auth) and Base64. */
#ifndef AURA_NET_AUTH_H
#define AURA_NET_AUTH_H

#include "../core/core.h"

typedef struct AuAuthParams {
    char scheme[8];   /* "Basic" or "Digest" */
    char realm[256];
    char nonce[256];
    char opaque[256];
    char algorithm[24]; /* "MD5", "SHA-256" (also the -sess forms) */
    bool qop_auth;
    bool stale;
    bool userhash;
} AuAuthParams;

/* Picks the strongest supported challenge from WWW-Authenticate (or
 * Proxy-Authenticate) values: Digest with SHA-256, then Digest with MD5,
 * then Basic. Returns false when none is supported. */
bool au_auth_parse(const char *value, AuAuthParams *out);
/* Appends "Basic base64(user:pass)". */
void au_auth_basic(const char *user, const char *pass, AuBuf *out);
/* Appends a Digest Authorization value. cnonce is the client nonce (hex
 * text); nc the nonce count. */
void au_auth_digest(const AuAuthParams *p, const char *user, const char *pass, const char *method, const char *uri,
                    uint32_t nc, const char *cnonce, AuBuf *out);

void au_base64_encode(const void *data, size_t len, AuBuf *out);
/* Decodes forgiving-base64 (the Infra standard: whitespace ignored,
 * padding optional). Returns false for invalid input. */
bool au_base64_decode(const char *s, size_t n, AuBuf *out);

#endif
