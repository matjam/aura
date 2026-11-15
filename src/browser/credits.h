/* What Aura is made of, for the About box and about:licences: the
 * version, the bundled fonts and the third-party libraries, each with
 * its licence. Portable C99. */
#ifndef AURA_BROWSER_CREDITS_H
#define AURA_BROWSER_CREDITS_H

#include <stddef.h>

#define AU_VERSION "0.3"
#define AU_TAGLINE "A web browser for Mac OS 9, with its engine written from scratch."

typedef enum { AU_CREDIT_FONT, AU_CREDIT_LIBRARY } AuCreditKind;

typedef struct AuCredit {
    AuCreditKind kind;
    const char *name;      /* as shown, with the version */
    const char *what;      /* what Aura uses it for */
    const char *copyright; /* the copyright line(s), "\n" between */
    const char *licence;   /* the licence's name */
    const char *file;      /* where the full licence text is, beside Aura */
} AuCredit;

extern const AuCredit au_credits[];
extern const size_t au_ncredits;

#endif
