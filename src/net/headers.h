/* HTTP header lists and the small parsers HTTP header values need
 * (token lists, quoted strings, parameters). Portable C99. */
#ifndef AURA_NET_HEADERS_H
#define AURA_NET_HEADERS_H

#include "../core/core.h"

/* An ordered list of fields. Names keep their case; lookups ignore it. */
typedef struct AuHeader {
    char *name, *value;
} AuHeader;

typedef struct AuHeaders {
    AuHeader *items;
    size_t n, cap;
} AuHeaders;

void au_headers_init(AuHeaders *h);
void au_headers_free(AuHeaders *h);
void au_headers_clear(AuHeaders *h);
void au_headers_copy(AuHeaders *dst, const AuHeaders *src); /* dst uninitialised or free */
void au_headers_add(AuHeaders *h, const char *name, const char *value);
void au_headers_addn(AuHeaders *h, const char *name, size_t nlen, const char *value, size_t vlen);
/* Replaces every field of that name with one (or adds it). */
void au_headers_set(AuHeaders *h, const char *name, const char *value);
void au_headers_remove(AuHeaders *h, const char *name);
/* The first value, or NULL. */
const char *au_headers_get(const AuHeaders *h, const char *name);
/* All values of a name joined with ", " into out (reset first). Returns
 * false when there is none. Set-Cookie must not be combined; iterate
 * with au_headers_next instead. */
bool au_headers_get_combined(const AuHeaders *h, const char *name, AuBuf *out);
/* Iterates the fields named name: start with *pos = 0. Returns NULL at the
 * end. */
const char *au_headers_next(const AuHeaders *h, const char *name, size_t *pos);
/* Whether the comma-separated list in the named fields contains token
 * (case-insensitive), as for Connection: close. */
bool au_headers_has_token(const AuHeaders *h, const char *name, const char *token);

/* ---- Value parsing ---- */

/* RFC 9110 tchar. */
bool au_http_is_tchar(int c);
/* Splits a comma-separated list: returns the next element (trimmed) in
 * item and ilen and advances *s; returns false at the end. Commas inside
 * quoted strings do not split. */
bool au_http_list_next(const char **s, const char **item, size_t *ilen);
/* Parses a MIME type ("text/html; charset=UTF-8") into its lowercased
 * essence and the charset parameter (as written, unquoted). Returns false
 * when the type is not type/subtype. */
bool au_http_parse_mime(const char *s, AuBuf *essence, AuBuf *charset);
/* Finds parameter name in a ";"-separated parameter list (case-insensitive
 * name) and appends its value (unquoted) to out. */
bool au_http_param(const char *s, const char *name, AuBuf *out);

/* HTTP dates (IMF-fixdate, RFC 850, asctime) to seconds since 1970 UTC.
 * Returns false when unparseable. */
bool au_http_parse_date(const char *s, int64_t *t);
/* Writes an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") into out. */
void au_http_format_date(int64_t t, char out[32]);
/* Days since 1970-01-01 for a civil date, and the inverse. */
int64_t au_days_from_civil(int64_t y, unsigned m, unsigned d);

#endif
