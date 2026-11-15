/* The CORS protocol's rules (Fetch Standard, sections 2 and 4.8 to 4.10),
 * as functions over names, values and header lists. No I/O and no state
 * but the preflight cache: src/js/fetch.c drives the requests (script's
 * fetch and XMLHttpRequest), and src/page uses the checks for elements
 * with a crossorigin attribute and for Cross-Origin-Resource-Policy.
 *
 * Header lists are name, value pairs (const char *const *, n pairs); names
 * compare without case. Origins are serialized ("https://example.com",
 * "null" for an opaque one). */
#ifndef AURA_NET_CORS_H
#define AURA_NET_CORS_H

#include "../core/core.h"

/* ---- Methods and request headers ---- */
/* CONNECT, TRACE, TRACK: script may not use them. */
bool au_cors_forbidden_method(const char *method);
/* GET, HEAD, POST: need no preflight. */
bool au_cors_safelisted_method(const char *method);
/* A header script may not set (Fetch "forbidden request-header"): the
 * names the user agent controls, Proxy-* and Sec-*, and the method
 * override headers when they name a forbidden method. */
bool au_cors_forbidden_request_header(const char *name, const char *value);
/* A header that needs no preflight: Accept, Accept-Language,
 * Content-Language, Content-Type (three types only) and a simple Range,
 * with a value of at most 128 bytes without unsafe bytes. */
bool au_cors_safelisted_request_header(const char *name, const char *value);
/* What a no-cors request may carry: the safelisted headers but Range. */
bool au_cors_no_cors_header(const char *name, const char *value);
/* Authorization: never covered by "Access-Control-Allow-Headers: *". */
bool au_cors_non_wildcard_request_header(const char *name);

/* ---- Response headers ---- */
/* Set-Cookie, Set-Cookie2: never shown to script. */
bool au_cors_forbidden_response_header(const char *name);
/* Whether a cors response shows this header to script: the safelisted
 * names (Cache-Control, Content-Language, Content-Length, Content-Type,
 * Expires, Last-Modified, Pragma) and those the response lists in
 * Access-Control-Expose-Headers ("*" lists all, without credentials). */
bool au_cors_exposed_response_header(const char *name, const char *const *headers, size_t n, bool credentials);

/* The first value of a header, or NULL. */
const char *au_cors_header(const char *const *headers, size_t n, const char *name);

/* ---- The CORS check ----
 * A response to a cross-origin request made with origin is readable when
 * Access-Control-Allow-Origin is that origin, or "*" for a request without
 * credentials; with credentials Access-Control-Allow-Credentials must be
 * "true" as well. */
bool au_cors_check(const char *origin, bool credentials, const char *const *headers, size_t n);

/* ---- Preflight ----
 * The value of Access-Control-Request-Headers for a request: the names of
 * its headers that are not safelisted, lower case, sorted, joined with
 * commas. Returns false (out empty) when there are none. */
bool au_cors_unsafe_header_names(const char *const *req_headers, size_t n, AuBuf *out);
/* Whether a preflight response allows the request: the CORS check, an ok
 * status, the method in Access-Control-Allow-Methods (or safelisted, or
 * "*" without credentials) and every unsafe header name in
 * Access-Control-Allow-Headers (or "*" without credentials, Authorization
 * always by name). why receives a short reason on refusal. */
bool au_cors_preflight_allows(const char *origin, bool credentials, const char *method,
                              const char *const *req_headers, size_t nreq, int status,
                              const char *const *resp_headers, size_t nresp, const char **why);
/* Access-Control-Max-Age in seconds: 5 when absent or invalid, capped at
 * AU_CORS_MAX_AGE (two hours, as Chromium caps it). */
#define AU_CORS_MAX_AGE 7200u
uint32_t au_cors_max_age(const char *const *resp_headers, size_t n);

/* ---- The preflight cache ----
 * Entries are keyed by origin, URL and credentials, and hold the methods
 * and header names a preflight allowed, until its max-age runs out. Times
 * are milliseconds on any monotonic clock. At most AU_CORS_CACHE_MAX
 * entries; the oldest goes first. */
#define AU_CORS_CACHE_MAX 32
typedef struct AuCorsCache AuCorsCache;
AuCorsCache *au_cors_cache_new(void);
void au_cors_cache_free(AuCorsCache *c);
void au_cors_cache_clear(AuCorsCache *c);
/* Records what a successful preflight response allows. */
void au_cors_cache_store(AuCorsCache *c, const char *origin, const char *url, bool credentials,
                         const char *const *resp_headers, size_t nresp, double now_ms);
/* Whether the cache already allows method and every unsafe header of the
 * request, so that no preflight is needed. */
bool au_cors_cache_allows(AuCorsCache *c, const char *origin, const char *url, bool credentials, const char *method,
                          const char *const *req_headers, size_t nreq, double now_ms);

/* ---- Origins ----
 * The serialized origin of an absolute URL: scheme, host and port for
 * http, https, ws, wss and ftp; "file://" for file: (Aura treats local
 * files as one origin among themselves); "null" otherwise. */
void au_cors_origin_of(const char *url, AuBuf *out);
/* Both URLs have the same non-opaque origin. */
bool au_cors_same_origin(const char *url_a, const char *url_b);

/* ---- Cross-Origin-Resource-Policy ----
 * Whether a no-cors response from url may be used by a document of
 * origin: "same-origin" needs the same origin, "same-site" the same
 * site (and no downgrade from https), anything else allows. corp may be
 * NULL. */
bool au_corp_allows(const char *corp, const char *origin, const char *url);

#endif
