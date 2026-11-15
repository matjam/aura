/* The cookie manager's model: the cookie jar as a list of sites
 * (registrable domains) with their cookies, for the Cookies window.
 * Portable; the shell puts the list view in a window, passes clicks and
 * buttons in, and writes the rules back into the "cookies.sites"
 * preference after a change (au_cookie_rules_serialize).
 *
 * Rows: one folder per site (with a rule or at least one cookie), sorted
 * by name, its detail the cookie count and the rule; under an expanded
 * site, one row per cookie, its detail the domain, path, flags and
 * expiry. A site row's id is AU_COOKIE_ROW_SITE plus the site's index;
 * a cookie row's id is its index in the jar. The rows are rebuilt after
 * every change, so ids are valid only until the next change. */
#ifndef AURA_NET_COOKIE_VIEW_H
#define AURA_NET_COOKIE_VIEW_H

#include "cookies.h"

#include "../browser/listview.h"

#define AU_COOKIE_ROW_SITE 0x80000000u

typedef struct AuCookieView {
    AuCookieJar *jar;
    AuListView list;
    char **sites; /* sorted site keys of the current rows */
    size_t nsites;
    char **open;  /* expanded site keys, kept across rebuilds */
    size_t nopen;
    int32_t utc_offset; /* seconds east of UTC, for expiry dates */
} AuCookieView;

void au_cookie_view_init(AuCookieView *v, AuCookieJar *jar);
void au_cookie_view_free(AuCookieView *v);
/* Rebuilds the rows from the jar, keeping the selection on the same
 * site where it can. now is seconds since 1970 (expired cookies are not
 * listed). */
void au_cookie_view_refresh(AuCookieView *v, int64_t now);
/* Expands or collapses a site row. */
void au_cookie_view_toggle(AuCookieView *v, int row, int64_t now);

/* The site key of a row (a site row, or a cookie row's site), or NULL. */
const char *au_cookie_view_site(const AuCookieView *v, int row);
/* The cookie of a row, or NULL for a site row. */
const AuCookie *au_cookie_view_cookie(const AuCookieView *v, int row);
/* Every field and flag of the row's cookie, or a summary of the site:
 * lines of "Label: value". out is reset. */
void au_cookie_view_describe(const AuCookieView *v, int row, AuBuf *out, int64_t now);

/* Removes the row's cookie, or all of a site's cookies for a site row.
 * Returns the number removed. */
size_t au_cookie_view_remove(AuCookieView *v, int row, int64_t now);
void au_cookie_view_remove_all(AuCookieView *v, int64_t now);
/* Sets the rule of the row's site (NONE clears it). */
void au_cookie_view_set_rule(AuCookieView *v, int row, AuCookieRule rule, int64_t now);

/* Formats seconds since 1970 as "2026-11-15 13:05". */
void au_cookie_format_time(int64_t t, char *out, size_t size);

#endif
