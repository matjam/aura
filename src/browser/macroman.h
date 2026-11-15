/* Mac Roman, the text encoding of window titles, menus, TextEdit and the
 * plain-text clipboard on Mac OS 9, and the conversions Aura makes to it
 * from UTF-8. Portable C99.
 *
 * Converting to Mac Roman is lossy. au_macroman_from_utf8 keeps every
 * character Mac Roman has, transliterates common ones it lacks (dashes,
 * quotes, spaces, primes, arrows, fractions, Latin letters with other
 * diacritics, fullwidth forms) to the nearest Mac Roman text, drops
 * invisible ones (zero-width spaces, soft hyphens, combining marks,
 * byte order marks), and replaces the rest with one character chosen by
 * the caller. Invalid UTF-8 is replaced the same way, so the result never
 * holds garbage. */
#ifndef AURA_BROWSER_MACROMAN_H
#define AURA_BROWSER_MACROMAN_H

#include "../core/core.h"

/* Mac Roman byte 0x80 + i is code point au_macroman_high[i]. */
extern const uint16_t au_macroman_high[128];

enum {
    AU_MR_CR_LINES = 1 /* line feeds (and CR LF) become carriage returns, as Mac text files and the clipboard use */
};

/* The Mac Roman byte for cp, or -1 when Mac Roman lacks it. */
int au_macroman_byte(uint32_t cp);
/* Appends the Mac Roman form of UTF-8 text s (n bytes) to out. missing
 * replaces characters with no transliteration (for example '?', or 0xA5,
 * the bullet). flags: AU_MR_*. */
void au_macroman_from_utf8(const char *s, size_t n, AuBuf *out, char missing, unsigned flags);
/* Appends Mac Roman bytes as UTF-8; carriage returns stay carriage
 * returns unless flags has AU_MR_CR_LINES, which turns them into line
 * feeds. */
void au_macroman_to_utf8(const char *s, size_t n, AuBuf *out, unsigned flags);
/* Appends UTF-16 big-endian code units (the clipboard's 'utxt' flavour)
 * for UTF-8 text, with line feeds as carriage returns when flags has
 * AU_MR_CR_LINES; invalid UTF-8 becomes U+FFFD. */
void au_utf16be_from_utf8(const char *s, size_t n, AuBuf *out, unsigned flags);

#endif
