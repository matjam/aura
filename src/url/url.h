/* Aura URL parser and serializer, following the WHATWG URL Standard.
 *
 * To parse, call au_url_parse with the input (UTF-8) and an optional base
 * URL; on success the AuUrl holds every component already serialized,
 * plus the whole serialization in href. Free it with au_url_free.
 *
 *     AuUrl u;
 *     if (au_url_parse(&u, "../a?b#c", &base)) {
 *         puts(u.href);
 *         au_url_free(&u);
 *     }
 *
 * Covered: special schemes and default ports, relative resolution, the
 * percent-encode sets, IPv4 (decimal, octal, hex, short forms) and IPv6
 * hosts, opaque hosts for non-special schemes, path normalization with
 * percent-encoded dot segments, Windows drive letters in file: URLs,
 * opaque paths (data:, about:, mailto:), query (with the special-query
 * set for special schemes) and fragment, and origin computation.
 *
 * Hosts and IDNA: special hosts are percent-decoded, then go through a
 * reduced UTS #46 ToASCII: ASCII is lowercased, the ideographic full
 * stops (U+3002, U+FF0E, U+FF61) become '.', and each label with
 * non-ASCII code points is Punycode-encoded with the "xn--" prefix;
 * labels that already start with "xn--" must decode as valid Punycode.
 * Missing: the UTS #46 mapping table (Unicode case folding, NFC, ignored
 * and disallowed code points other than the forbidden set) and the
 * bidi and joiner rules, so "Ⓐ.com" or "FAß.de" are not mapped as a
 * browser would map them (they are encoded unmapped, apart from ASCII
 * lowercase). Forbidden domain code points are rejected.
 *
 * Not covered: the setters' state override (URL API setters come with
 * the JavaScript bindings) and blob: URL origins beyond parsing the path
 * as a URL.
 *
 * Limits: input longer than AU_URL_MAX_INPUT bytes is refused. */
#ifndef AURA_URL_H
#define AURA_URL_H

#include "../core/core.h"

#define AU_URL_MAX_INPUT (8u * 1024 * 1024)

typedef enum {
    AU_URL_HOST_NULL,   /* no host (mailto:x, data:...) */
    AU_URL_HOST_EMPTY,  /* the empty host (file:///p) */
    AU_URL_HOST_DOMAIN, /* an ASCII domain */
    AU_URL_HOST_IPV4,
    AU_URL_HOST_IPV6,
    AU_URL_HOST_OPAQUE  /* non-special scheme's host */
} AuUrlHostType;

typedef struct AuUrl {
    char *href;     /* the serialization */
    char *scheme;   /* lower case, without ':' */
    char *username; /* "" when none */
    char *password; /* "" when none */
    char *host;     /* serialized host (IPv6 in brackets); "" when null or empty */
    char *path;     /* serialized path ("/a/b", or the opaque path) */
    char *query;    /* without '?'; NULL when none */
    char *fragment; /* without '#'; NULL when none */
    int port;       /* -1 when none or the scheme's default */
    AuUrlHostType host_type;
    bool special;      /* http, https, ws, wss, ftp, file */
    bool opaque_path;  /* "cannot be a base URL" */
    uint32_t ipv4;     /* when host_type is IPV4 */
    uint16_t ipv6[8];  /* when host_type is IPV6 */
} AuUrl;

/* Parses input against base (NULL for none). Returns false for a failure
 * (u is then empty and needs no au_url_free, though calling it is safe). */
bool au_url_parse(AuUrl *u, const char *input, const AuUrl *base);
bool au_url_parse_n(AuUrl *u, const char *input, size_t len, const AuUrl *base);
void au_url_free(AuUrl *u);
void au_url_copy(AuUrl *dst, const AuUrl *src);

/* Parses rel against the base URL string base (NULL for none) and returns
 * the serialization (au_alloc'd), or NULL on failure. */
char *au_url_resolve_str(const char *base, const char *rel);

/* The scheme's default port, or -1. */
int au_url_default_port(const char *scheme);
/* The port in effect: the explicit one or the default (-1 for none). */
int au_url_effective_port(const AuUrl *u);
/* href without the fragment, into out (reset first). */
void au_url_serialize_no_fragment(const AuUrl *u, AuBuf *out);
bool au_url_equal_no_fragment(const AuUrl *a, const AuUrl *b);

/* ---- Origins ----
 * A tuple origin (scheme, host, port) for http, https, ws, wss and ftp
 * (and blob: URLs whose path parses as one of those); every other URL
 * has an opaque origin, which is equal to nothing, not even itself. */
typedef struct AuOrigin {
    bool opaque;
    char scheme[16];
    char *host; /* au_alloc'd; NULL when opaque */
    int port;   /* effective port */
} AuOrigin;

void au_url_origin(const AuUrl *u, AuOrigin *o);
void au_origin_free(AuOrigin *o);
bool au_origin_same(const AuOrigin *a, const AuOrigin *b);
/* "https://example.com", "http://h:8080", or "null". */
void au_origin_serialize(const AuOrigin *o, AuBuf *out);

/* ---- Percent-encoding ---- */
typedef enum {
    AU_PCT_C0,
    AU_PCT_FRAGMENT,
    AU_PCT_QUERY,
    AU_PCT_SPECIAL_QUERY,
    AU_PCT_PATH,
    AU_PCT_USERINFO,
    AU_PCT_COMPONENT,
    AU_PCT_FORM /* application/x-www-form-urlencoded (space stays; the caller maps it to '+') */
} AuPctSet;

bool au_pct_in_set(unsigned c, AuPctSet set);
/* Appends bytes s[0..n) to out, percent-encoding bytes in the set. */
void au_pct_encode(AuBuf *out, const char *s, size_t n, AuPctSet set);
/* Appends the percent-decoding of s[0..n) to out (bytes). */
void au_pct_decode(AuBuf *out, const char *s, size_t n);

/* ---- Hosts ----
 * Parses a host string as the URL parser does for a special (or, with
 * special false, non-special) scheme, and appends the serialization to
 * out. Returns false for an invalid host. */
bool au_url_host_parse(const char *s, size_t n, bool special, AuBuf *out, AuUrlHostType *type);
/* Punycode (RFC 3492) of a sequence of code points; appends to out.
 * Returns false on overflow. */
bool au_punycode_encode(const uint32_t *cp, size_t n, AuBuf *out);
/* Decodes Punycode (without "xn--") into code points (at most cap).
 * Returns the count, or -1 when invalid. */
int au_punycode_decode(const char *s, size_t n, uint32_t *out, size_t cap);

#endif
