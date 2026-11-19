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
#define AU_VERSION "0.9.1" /* x-release-please-version */
#define AU_TAGLINE "A web browser for Mac OS 9, with its engine written from scratch."
#define AU_AUTHOR "Nathan Ollerenshaw"
#define AU_HOMEPAGE "https://github.com/matjam/aura"
/* What Aura says it is (au_prefs_user_agent). By default the truth: the
 * platform the build runs on (core/uaplatform.h) and Aura's name and
 * version. Claiming Chrome while lacking what Chrome has (WebAssembly,
 * WebGL, WebRTC) marks a browser as a bot to risk checks such as
 * hCaptcha's. The user can choose Chrome's or Safari's string instead,
 * for sites that refuse browsers they do not know. AU_UA_CHROME_VERSION
 * is the current Chrome major version when Aura is released: update it
 * with each release, since sites also turn away old Chrome versions. */
#define AU_UA_CHROME_VERSION "156"
/* Safari's version for AU_SAFARI_USER_AGENT; update it with Chrome's. */
#define AU_UA_SAFARI_VERSION "26.0"
/* Aura's own user agent (Preferences > Network > Identify as: Aura): the
 * platform it runs on and Aura's name and version, claiming no other
 * browser. */
#define AU_DEFAULT_USER_AGENT "Mozilla/5.0 (" AU_UA_PLATFORM ") Aura/" AU_VERSION
/* Chrome's and Safari's strings, for sites that turn away browsers they
 * do not know. Chrome does not run on Mac OS 9 and Safari only on macOS:
 * where the mimicked browser does not run, the string names macOS, which
 * it does. */
#if AU_UA_KIND == AU_UA_KIND_MACOS9
#define AU_UA_CHROME_PLATFORM "Macintosh; Intel Mac OS X 10_15_7"
#else
#define AU_UA_CHROME_PLATFORM AU_UA_PLATFORM
#endif
#define AU_CHROME_USER_AGENT                                                                                       \
    "Mozilla/5.0 (" AU_UA_CHROME_PLATFORM ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" AU_UA_CHROME_VERSION \
    ".0.0.0 Safari/537.36"
#define AU_SAFARI_USER_AGENT                                                                                 \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/" \
    AU_UA_SAFARI_VERSION " Safari/605.1.15"
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
