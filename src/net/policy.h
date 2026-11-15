/* Request policy helpers: Referrer-Policy, Fetch Metadata (Sec-Fetch-*),
 * Accept values, mixed content classification, and MIME sniffing with
 * X-Content-Type-Options: nosniff. */
#ifndef AURA_NET_POLICY_H
#define AURA_NET_POLICY_H

#include "net.h"
#include "psl.h"

/* ---- Referrer Policy ---- */
AuReferrerPolicy au_referrer_policy_token(const char *s, size_t n); /* AU_RP_DEFAULT when unknown */
/* Parses a Referrer-Policy header value; unknown tokens are ignored and
 * the last known one wins; returns current when none is known. */
AuReferrerPolicy au_referrer_policy_parse(const char *value, AuReferrerPolicy current);
/* The Referer value for a request to target from a document at referrer
 * under policy. Returns false when no Referer is sent. */
bool au_referrer_compute(const char *referrer, const AuUrl *target, AuReferrerPolicy policy, AuBuf *out);
/* "Potentially trustworthy URL": https, wss, file, loopback, localhost. */
bool au_url_trustworthy(const AuUrl *u);

/* ---- Fetch Metadata ---- */
const char *au_sec_fetch_site(const char *initiator, const AuUrl *target); /* none/same-origin/same-site/cross-site */
const char *au_sec_fetch_dest(AuRequestDest d);
const char *au_sec_fetch_mode(AuRequestMode m);
const char *au_accept_for(AuRequestDest d);

/* ---- Mixed content (W3C Mixed Content Level 2) ----
 * For a subresource of a secure document: AU_MIXED_NONE when the request
 * is not mixed; UPGRADEABLE for images (and audio/video), which are
 * upgraded to https before fetching; BLOCKABLE for everything else
 * (scripts, styles, fonts, frames, fetch), which is blocked. */
typedef enum { AU_MIXED_NONE, AU_MIXED_UPGRADEABLE, AU_MIXED_BLOCKABLE } AuMixedClass;
AuMixedClass au_mixed_classify(const AuUrl *document, const AuUrl *target, AuRequestDest dest);
/* Rewrites an http URL to https (port 80 becomes the default). */
void au_url_upgrade_to_https(AuUrl *u);

/* ---- MIME sniffing (WHATWG MIME Sniffing) ----
 * The computed MIME type of a resource from its Content-Type (supplied,
 * may be "" when absent), the nosniff flag and the first bytes. Returns a
 * static string. For documents (AU_DEST_DOCUMENT, IFRAME) it follows
 * "rules for identifying an unknown MIME type" when the type is absent or
 * unknown; for images it sniffs image signatures; style sheets and scripts
 * never sniff: with nosniff, au_mime_allowed says whether the type is
 * acceptable at all. */
const char *au_mime_sniff(const char *supplied, bool nosniff, const uint8_t *data, size_t len, AuRequestDest dest);
/* With nosniff: a style sheet must be text/css, a script a JavaScript
 * MIME type; other destinations are allowed. Without nosniff everything
 * is allowed (style sheets of other types are still refused by the page
 * code in standards mode). */
bool au_mime_allowed(const char *mime, bool nosniff, AuRequestDest dest);
bool au_mime_is_javascript(const char *mime);
/* Bytes needed for a sniffing decision (the standard's resource header). */
#define AU_SNIFF_BYTES 1445

/* ---- Encoding detection for documents ----
 * The encoding of an HTML document from, in order: a byte order mark,
 * the Content-Type charset, the <meta charset> prescan of the first 1024
 * bytes, then windows-1252. Returns a canonical name from
 * au_encoding_label ("UTF-8", "UTF-16LE", "UTF-16BE", "windows-1252",
 * or another Encoding Standard name that Aura cannot decode yet). */
const char *au_detect_encoding(const uint8_t *data, size_t len, const char *charset);
/* The Encoding Standard's name for a label ("latin1" -> "windows-1252"),
 * or NULL for an unknown label. Covers every label in the standard's
 * table (names only; Aura decodes UTF-8, UTF-16 and windows-1252). */
const char *au_encoding_label(const char *label, size_t n);
/* The label table, for tests: its size and the i-th label (and name). */
size_t au_encoding_label_count(void);
const char *au_encoding_label_at(size_t i, const char **name);

#endif
