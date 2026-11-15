/* Site icons for tabs and lists.
 *
 * One store per browser holds a 16 x 16 icon per host (http and https
 * sites). When a tab's document has loaded, the browser asks the page for
 * its icon URL (au_page_icon_url: the best <link rel=icon>, else
 * /favicon.ico) and au_favicons_request fetches it through the scheme's
 * fetcher as an image subresource of the page, so the blocklist, mixed
 * content blocking and the HTTP cache apply; an icon on another host is
 * fetched without cookies. The bytes are decoded with
 * au_image_decode_favicon. A host is asked again after
 * AU_FAVICON_FRESH_S; a host that gave no icon is not asked again while
 * the application runs.
 *
 * The store serializes to a small file (at most AU_FAVICON_MAX hosts,
 * about 1 KB each), which the shell keeps beside the history. It names
 * hosts the user visited: clear it with the history
 * (au_favicons_clear). */
#ifndef AURA_BROWSER_FAVICON_H
#define AURA_BROWSER_FAVICON_H

#include "core/core.h"
#include "paint/paint.h"

#include "history.h" /* AuTime */
#include "page.h"

#define AU_FAVICON_SIZE 16
#define AU_FAVICON_MAX 200
#define AU_FAVICON_FRESH_S (7 * 24 * 3600)
#define AU_FAVICON_MAX_BYTES (512u * 1024u) /* a larger icon file is dropped */
#define AU_FAVICON_FETCHES 4                /* at a time */

typedef struct AuFavicons AuFavicons;

AuFavicons *au_favicons_new(void);
void au_favicons_free(AuFavicons *f);

/* The icon of url's host, or NULL (no icon known, or not http or https).
 * Valid until the next call that changes the store. */
const AuSurface *au_favicons_for_url(AuFavicons *f, const char *url);

/* The same by host name (lower case), and the host an icon is filed
 * under for url (false when url is not http or https). */
const AuSurface *au_favicons_for_host(AuFavicons *f, const char *host);
bool au_favicons_host(const char *url, char *host, size_t cap);

/* Stores icon (any size; scaled to 16 x 16 if needed is the caller's
 * job: pass a 16 x 16 surface) for host, replacing an older one; the
 * least recently fetched host goes when the store is full. */
void au_favicons_put(AuFavicons *f, const char *host, const AuSurface *icon, AuTime when);

/* Starts fetching icon_url for the page at page_url unless its host has a
 * fresh icon, was already found to have none, or is being fetched.
 * fetcher is the icon URL's scheme's (NULL: nothing happens). */
void au_favicons_request(AuFavicons *f, const AuPageFetcher *fetcher, const char *page_url, const char *icon_url,
                         AuTime now);
/* Call from the idle loop: polls the fetchers of fetches under way.
 * Returns a number that changes whenever an icon arrives or the store is
 * cleared (repaint the tabs when it differs from the last one seen). */
uint32_t au_favicons_idle(AuFavicons *f);
/* Fetches under way. */
int au_favicons_pending(const AuFavicons *f);
/* Cancels the fetches under way (before the fetchers go away). */
void au_favicons_cancel(AuFavicons *f);

/* Persistence. au_favicons_parse replaces the store's contents and
 * returns false for data it does not understand (the store is then
 * empty). */
bool au_favicons_dirty(const AuFavicons *f);
void au_favicons_serialize(AuFavicons *f, AuBuf *out); /* clears dirty */
bool au_favicons_parse(AuFavicons *f, const void *data, size_t len);
void au_favicons_clear(AuFavicons *f);
size_t au_favicons_count(const AuFavicons *f);

#endif
