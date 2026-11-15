/* Preferences to network configuration. */
#ifndef AURA_NET_PREFS_H
#define AURA_NET_PREFS_H

#include "net.h"
#include "../browser/prefs.h"

/* Applies user agent, DNT, GPC, HTTPS-first, the cookie policy and
 * third-party blocking (also to the attached cookie jar), and the proxies.
 * Call again after the preferences change. */
void au_net_apply_prefs(AuNet *net, const AuPrefs *p);
/* The memory cache size the preferences ask for, in bytes. */
size_t au_net_cache_bytes_from_prefs(const AuPrefs *p);

#endif
