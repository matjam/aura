/* Address-field URL helpers. Portable C99.
 *
 * This is deliberately limited: it decides whether address-field input is
 * a URL, a local file path or a search, and converts between file: URLs
 * and platform paths. It is not a URL parser. It does not resolve relative
 * references, normalise paths, decode hosts (IDNA) or validate ports; the
 * WHATWG URL parser that does those things comes with networking and
 * replaces the parts of this file that overlap with it.
 *
 * Rules au_url_from_input applies to trimmed input, in order:
 *   1. Empty input gives NULL.
 *   2. A local path gives a file: URL: a POSIX path ("/Users/me/a.html")
 *      or an HFS path, which has a ':' and no '/' and is not host:port
 *      ("Macintosh HD:Documents:a.html").
 *   3. Input with a scheme is a URL as typed. A scheme is recognised when
 *      it is one Aura knows (http, https, file, about, ftp, gopher, data,
 *      mailto, javascript, view-source) or when "//" follows the ':'.
 *   4. Input with no spaces whose first part (up to '/', '?' or '#') looks
 *      like a host gets "http://" in front. A host is "localhost", an IPv4
 *      address, a bracketed IPv6 address, or dot-separated labels of
 *      letters, digits and '-' whose last label is at least two letters;
 *      any of these may carry ":port".
 *   5. Anything else is a search: the template's "%s" is replaced by the
 *      input, form-encoded (space as '+'). A template without "%s" gets
 *      the query appended. */
#ifndef AURA_BROWSER_URL_H
#define AURA_BROWSER_URL_H

#include "../core/core.h"

/* The scheme length when s starts with "scheme:" (RFC 3986 characters:
 * a letter, then letters, digits, '+', '-', '.'), else 0. */
size_t au_url_scheme_len(const char *s, size_t n);
/* Whether url's scheme is scheme (ASCII case-insensitive). */
bool au_url_scheme_is(const char *url, const char *scheme);
/* Whether s (n bytes, optionally with ":port") looks like a host by
 * rule 4 above. */
bool au_url_looks_like_host(const char *s, size_t n);

/* Turns address-field text into a URL by the rules above. Returns a
 * string to free with au_free, or NULL for empty input. *is_search, when
 * not NULL, says whether rule 5 applied. */
char *au_url_from_input(const char *input, const char *search_template, bool *is_search);

/* Appends s form-encoded (application/x-www-form-urlencoded) to out. */
void au_url_form_encode(AuBuf *out, const char *s, size_t n);
/* Appends s percent-decoded to out ("%41" is 'A'; a bad escape is copied
 * as it is). */
void au_url_percent_decode(AuBuf *out, const char *s, size_t n);

/* A file: URL for a platform path. sep is the path separator: '/' for a
 * POSIX path (which starts with '/'), ':' for an HFS path (which starts
 * with the volume name). Returns a string to free with au_free. */
char *au_url_from_path(const char *path, char sep);
/* The platform path for a file: URL ("file:///a/b" or "file://localhost/a/b"),
 * percent-decoded, with sep as the separator. NULL when url is not a
 * file: URL with a path. Free with au_free. */
char *au_url_to_path(const char *url, char sep);

/* The part of a URL that address-field completion matches against: the
 * URL without its scheme, "//" and a leading "www.". Returns a pointer
 * into url and stores the length in *len. */
const char *au_url_match_key(const char *url, size_t *len);

/* The host of a URL with "scheme://host...", copied into out (cap bytes,
 * NUL-terminated); "" when there is none. */
void au_url_host(const char *url, char *out, size_t cap);

#endif
