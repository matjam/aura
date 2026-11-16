/* The cookie store (RFC 6265bis): parsing Set-Cookie, matching, SameSite,
 * name prefixes, third-party blocking, partitioned cookies (CHIPS),
 * limits and eviction, a persistent text format, and document.cookie.
 *
 * Every question to the store comes with an AuCookieRequest describing
 * the request: its URL, the site for cookies (the top-level document's
 * URL; NULL when the request is the top-level navigation itself), the
 * initiator (the document that started a navigation; NULL when the user
 * did), whether it is a top-level navigation, and whether the method is
 * safe. From these the store decides:
 *
 *   - third party: the request's registrable domain (Public Suffix List)
 *     differs from the top-level document's. With third-party blocking on
 *     (the default preference), such cookies are neither sent nor stored,
 *     except cookies set with "Partitioned; Secure", which are stored
 *     keyed by the top-level site and sent only under that same site.
 *   - SameSite: absent means Lax. Strict cookies go only with same-site
 *     requests; Lax also with cross-site top-level navigations by a safe
 *     method; None (which requires Secure) with any request that is not
 *     blocked as third party.
 *
 * Limits: a cookie's name and value at most 4096 bytes together, an
 * attribute value at most 1024 bytes, at most AU_COOKIES_PER_DOMAIN per
 * registrable domain and AU_COOKIES_TOTAL in all (least recently used
 * evicted first, expired cookies before anything), expiry at most 400
 * days ahead. */
#ifndef AURA_NET_COOKIES_H
#define AURA_NET_COOKIES_H

#include "net.h"
#include "../url/url.h"

#define AU_COOKIES_PER_DOMAIN 150
#define AU_COOKIES_TOTAL 3000
#define AU_COOKIE_MAX_AGE_SECONDS (400LL * 86400)

typedef enum { AU_SAMESITE_DEFAULT, AU_SAMESITE_NONE, AU_SAMESITE_LAX, AU_SAMESITE_STRICT } AuSameSite;

typedef struct AuCookie {
    char *name, *value;
    char *domain;    /* lower case, no leading dot */
    char *path;
    char *partition; /* top-level site ("https://example.com") for a partitioned cookie; "" otherwise */
    int64_t expires; /* seconds since 1970; 0 for a session cookie */
    int64_t creation, last_access;
    bool host_only, secure, http_only, partitioned;
    AuSameSite same_site;
} AuCookie;

typedef struct AuCookieRequest {
    const AuUrl *url;
    const AuUrl *site_for_cookies; /* top-level document; NULL: url is the top level */
    const AuUrl *initiator;        /* who started a navigation; NULL: the user */
    bool top_level_nav;
    bool safe_method;              /* GET, HEAD, OPTIONS, TRACE */
    bool from_script;              /* document.cookie: no HttpOnly access */
    int64_t now;                   /* seconds since 1970 */
} AuCookieRequest;

AuCookieJar *au_cookie_jar_new(void);
void au_cookie_jar_free(AuCookieJar *jar);
void au_cookie_jar_set_policy(AuCookieJar *jar, AuNetCookiePolicy policy, bool block_third_party);
/* The policy in force, and a counter that moves whenever the policy or a
 * per-site rule changes (web storage follows both: web_storage.h). */
AuNetCookiePolicy au_cookie_jar_policy(const AuCookieJar *jar, bool *block_third_party);
uint32_t au_cookie_jar_policy_gen(const AuCookieJar *jar);

/* Stores the cookie in one Set-Cookie value. Returns true when stored (or
 * when it deleted a cookie by expiring it); false when ignored. why, if
 * not NULL, receives a short reason. */
bool au_cookie_jar_set(AuCookieJar *jar, const AuCookieRequest *rq, const char *set_cookie, const char **why);
/* The Cookie header value for the request ("a=1; b=2"); out is reset;
 * returns false when no cookie applies. */
bool au_cookie_jar_header(AuCookieJar *jar, const AuCookieRequest *rq, AuBuf *out);
/* Whether the request counts as third party (the decision above). */
bool au_cookie_is_third_party(const AuCookieRequest *rq);

/* document.cookie getter and setter (from_script is implied). */
void au_cookie_jar_document_get(AuCookieJar *jar, const AuCookieRequest *rq, AuBuf *out);
void au_cookie_jar_document_set(AuCookieJar *jar, const AuCookieRequest *rq, const char *value);

/* Drops session cookies (at quit). */
void au_cookie_jar_end_session(AuCookieJar *jar);
void au_cookie_jar_clear(AuCookieJar *jar);
/* The cookies manager's view. */
size_t au_cookie_jar_count(const AuCookieJar *jar);
const AuCookie *au_cookie_jar_at(const AuCookieJar *jar, size_t i);
void au_cookie_jar_remove_at(AuCookieJar *jar, size_t i);
/* Removes the cookies of a host and its subdomains. */
void au_cookie_jar_remove_domain(AuCookieJar *jar, const char *domain);

/* Per-site rules, keyed by registrable domain (the host itself when it
 * has none), set by the user in the cookie manager:
 *   - ALLOW: stored and sent even when cookies are off or the request is
 *     third party;
 *   - SESSION: stored as session cookies (never written to disk); setting
 *     the rule turns the site's stored cookies into session cookies;
 *   - BLOCK: neither stored nor sent; setting it removes the site's
 *     cookies.
 * NONE removes a rule. The text form, kept in the "cookies.sites"
 * preference, is "site=allow|session|block" items separated by spaces;
 * parse replaces all rules. */
typedef enum { AU_COOKIE_RULE_NONE, AU_COOKIE_RULE_ALLOW, AU_COOKIE_RULE_SESSION, AU_COOKIE_RULE_BLOCK } AuCookieRule;
void au_cookie_jar_set_rule(AuCookieJar *jar, const char *site, AuCookieRule rule);
AuCookieRule au_cookie_jar_rule(const AuCookieJar *jar, const char *host);
void au_cookie_rules_parse(AuCookieJar *jar, const char *text);
void au_cookie_rules_serialize(const AuCookieJar *jar, AuBuf *out);
size_t au_cookie_rules_count(const AuCookieJar *jar);
const char *au_cookie_rules_at(const AuCookieJar *jar, size_t i, AuCookieRule *rule);
const char *au_cookie_rule_name(AuCookieRule rule); /* "allow", "session", "block"; "" for NONE */
/* The rule key of a host or cookie domain (lower case registrable domain). */
void au_cookie_site_key(const char *host, AuBuf *out);

/* Persistent file (session cookies are not written; expired ones are
 * dropped). Format: UTF-8 text; first line "# Aura cookies 1"; then one
 * cookie per line, tab-separated:
 *   domain host_only path secure http_only same_site partitioned partition
 *   expires creation last_access name value
 * with flags as 0/1, same_site as default/none/lax/strict, times in
 * decimal seconds, and '%', tab, CR and LF in fields written as %25,
 * %09, %0D, %0A. The platform writes it safely (temporary file, then
 * swap). Returns false from parse for a damaged file (good lines are
 * kept). */
void au_cookie_jar_serialize(const AuCookieJar *jar, AuBuf *out, int64_t now);
bool au_cookie_jar_parse(AuCookieJar *jar, const char *data, size_t len, int64_t now);
/* Set whenever the persistent contents change; the shell saves and
 * clears it. */
bool au_cookie_jar_dirty(const AuCookieJar *jar);
void au_cookie_jar_clear_dirty(AuCookieJar *jar);

#endif
