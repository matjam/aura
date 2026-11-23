/* What Aura is made of, for the About box and about:licences: the
 * version, the bundled fonts and the third-party libraries, each with
 * its licence. Portable C99. */
#ifndef AURA_BROWSER_CREDITS_H
#define AURA_BROWSER_CREDITS_H

#include <stddef.h>

#include "core/uaplatform.h"

/* The version, for the About box, the user agent, the 'vers' resources and
 * the release file names. release-please rewrites this line when it
 * prepares a release (release-please-config.json); do not edit it by hand
 * between releases. */
#define AU_VERSION "0.10.0" /* x-release-please-version */
#define AU_TAGLINE "A web browser for Mac OS 9, with its engine written from scratch."
#define AU_AUTHOR "Nathan Ollerenshaw"
#define AU_HOMEPAGE "https://github.com/matjam/aura"
/* What Aura says it is (au_prefs_user_agent): a browser (Preferences >
 * Network > Identify as) on a platform (Platform). By default the truth:
 * Aura on the platform it runs on (au_ua_platform, core/core.h).
 * Claiming Chrome while lacking what Chrome has (WebAssembly, WebGL,
 * WebRTC) marks a browser as a bot to risk checks such as hCaptcha's;
 * Chrome's and Safari's strings are there for sites that refuse browsers
 * they do not know. AU_UA_CHROME_VERSION is the current Chrome major
 * version when Aura is released, and AU_UA_SAFARI_VERSION Safari's:
 * update them with each release, since sites also turn away old
 * versions. */
#define AU_UA_CHROME_VERSION "156"
#define AU_UA_SAFARI_VERSION "26.0"
#define AU_UA_FIREFOX_VERSION "158.0"
/* Aura's own user agent with the platform part built in; the run-time
 * string (au_prefs_user_agent) uses the platform the port found. */
#define AU_DEFAULT_USER_AGENT "Mozilla/5.0 (" AU_UA_PLATFORM ") Aura/" AU_VERSION
/* The three strings, each with %s for the platform part. */
#define AU_AURA_UA_FORMAT "Mozilla/5.0 (%s) Aura/" AU_VERSION
#define AU_CHROME_UA_FORMAT \
    "Mozilla/5.0 (%s) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" AU_UA_CHROME_VERSION ".0.0.0 Safari/537.36"
#define AU_SAFARI_UA_FORMAT \
    "Mozilla/5.0 (%s) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/" AU_UA_SAFARI_VERSION " Safari/605.1.15"
#define AU_FIREFOX_UA_FORMAT \
    "Mozilla/5.0 (%s; rv:" AU_UA_FIREFOX_VERSION ") Gecko/20100101 Firefox/" AU_UA_FIREFOX_VERSION
/* The platform part Chrome, Safari and Firefox send from any Mac, which
 * they froze (Firefox writes 10.15 with a dot); with "This computer" they
 * send it from a Mac, as the real browsers do. */
#define AU_UA_FROZEN_MAC "Macintosh; Intel Mac OS X 10_15_7"
/* The Chrome-shaped default versions up to 0.9 sent, with Aura's token at
 * the end; a saved custom string of this form is "Aura" again. */
#define AU_CHROME_DEFAULT_SUFFIX " Safari/537.36 Aura/"
/* The Mac OS 9 default, which earlier versions on every platform sent; a
 * saved preference holding it (or its form for another version) is "use
 * the default". */
#define AU_MACOS9_USER_AGENT_PREFIX "Mozilla/5.0 (Macintosh; PPC Mac OS 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/"
/* The plain form Aura sent up to version 0.3; a saved preference holding
 * it is treated as "use the default". */
#define AU_OLD_USER_AGENT_PREFIX "Mozilla/5.0 (Macintosh; U; PPC Mac OS 9) Aura/"

/* Data is material compiled into Aura or shipped beside it that is not
 * code: the root certificates, the Public Suffix List, the Brotli
 * dictionary. */
typedef enum { AU_CREDIT_FONT, AU_CREDIT_LIBRARY, AU_CREDIT_DATA } AuCreditKind;

typedef struct AuCredit {
    AuCreditKind kind;
    const char *name;      /* as shown, with the version */
    const char *what;      /* what Aura uses it for */
    const char *copyright; /* the copyright line(s), "\n" between */
    const char *licence;   /* the licence's name */
    const char *file;      /* where the full licence text is, beside Aura; NULL when text holds it */
    const char *text;      /* the licence text or notice, shown in about:licences; NULL for none */
} AuCredit;

extern const AuCredit au_credits[];
extern const size_t au_ncredits;

#endif
