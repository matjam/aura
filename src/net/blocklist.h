/* Domain blocklists: requests to listed hosts are never made.
 *
 * A list is a set of domains. A host is listed when it, or any parent
 * domain down to its registrable domain (never a bare public suffix), is
 * in the set, unless an allow entry covers it the same way.
 *
 * Memory is the constraint on Mac OS 9 (published lists hold 90 thousand
 * to several hundred thousand domains), so a compiled list keeps no
 * names: it is a sorted array of 48-bit hashes of the lower-case domain,
 * searched by bisection. Two different domains with the same hash cannot
 * be told apart: with n entries, one probe of an unlisted name hits by
 * chance with probability n / 2^48 (3.2e-10 for 91 thousand entries,
 * 5.2e-9 for 1.45 million), and a host costs two or three probes. A
 * false hit blocks an innocent host; the user's allow entries are kept
 * as text and override it, as does the per-site switch.
 *
 * The compiled file is the in-memory form (all integers big-endian):
 *
 *   0   4  "AuBL"
 *   4   2  version (1)
 *   6   2  hash bytes (6)
 *   8   4  block entries
 *   12  4  allow entries
 *   16  4  FNV-1a 32 of the entry bytes
 *   20  4  0 (reserved)
 *   24  64 the list's title, UTF-8, NUL-padded
 *   88     block entries (6 bytes each, strictly ascending), then allow
 *          entries (the same)
 *
 * Text lists are parsed by AuBlockBuilder, which accepts, line by line
 * and in any mix: plain domains, hosts files ("0.0.0.0 domain"), adblock
 * domain rules ("||domain^", exceptions "@@||domain^"), wildcard domains
 * ("*.domain"), dnsmasq rules ("local=/domain/"), and ignores comments
 * and everything else. Lines are hostile input: they are capped at
 * AU_BLOCK_LINE_MAX bytes, names are validated and lower-cased, and
 * names with non-ASCII characters go through the URL parser's IDNA
 * (Punycode) conversion.
 *
 * Large blocks (the builder's chunks, a compiled list) come from an
 * AuBlockMem, so the Mac OS 9 shell can take them from temporary memory;
 * NULL uses au_try_alloc. Every allocation may fail: the builder then
 * fails and the old list stays. */
#ifndef AURA_NET_BLOCKLIST_H
#define AURA_NET_BLOCKLIST_H

#include "../core/core.h"
#include "../url/url.h"

#define AU_BLOCK_HASH_BYTES 6
#define AU_BLOCK_HEADER 88
#define AU_BLOCK_TITLE_MAX 64 /* with the NUL */
#define AU_BLOCK_LINE_MAX 512
#define AU_BLOCK_MAX_ENTRIES 4000000u /* per list */
#define AU_BLOCK_MAX_LISTS 16

typedef struct AuBlockMem {
    void *ctx;
    void *(*alloc)(void *ctx, size_t n); /* NULL when there is no memory */
    void (*release)(void *ctx, void *p);
} AuBlockMem;

/* The 48-bit hash of a domain (lower case ASCII, no trailing dot). */
uint64_t au_block_hash(const char *domain, size_t n);

/* ---- Compiling ---- */
typedef struct AuBlockBuilder AuBlockBuilder;

typedef struct AuBlockBuildStats {
    uint32_t lines;    /* lines seen */
    uint32_t accepted; /* names taken (before duplicates are removed) */
    uint32_t allowed;  /* of them, exceptions */
    uint32_t ignored;  /* lines that were neither a comment nor a rule */
    uint32_t too_long; /* lines over AU_BLOCK_LINE_MAX */
} AuBlockBuildStats;

AuBlockBuilder *au_block_builder_new(const AuBlockMem *mem);
void au_block_builder_free(AuBlockBuilder *b);
/* Feeds list text, in pieces of any size. Returns false once the builder
 * has failed (out of memory, or more than AU_BLOCK_MAX_ENTRIES). */
bool au_block_builder_feed(AuBlockBuilder *b, const void *bytes, size_t len);
/* Adds one name or one hash directly (the personal lists, tests). */
bool au_block_builder_add(AuBlockBuilder *b, const char *domain, size_t n, bool allow);
bool au_block_builder_add_hash(AuBlockBuilder *b, uint64_t hash, bool allow);
/* The title for the compiled file; without one, the list's own "Title:"
 * comment is used. */
void au_block_builder_set_title(AuBlockBuilder *b, const char *title);
/* After the last feed: sorts and merges, writing at most budget entries
 * per call (0: all), so a shell can spread the work over idle time.
 * Returns 1 when the compiled list is ready, 0 when there is more to do,
 * -1 on failure. */
int au_block_builder_finish(AuBlockBuilder *b, uint32_t budget);
/* The compiled file (allocated from the builder's AuBlockMem; the caller
 * owns it and releases it there, or hands it to au_blocklist_attach). */
bool au_block_builder_take(AuBlockBuilder *b, uint8_t **data, size_t *len);
void au_block_builder_stats(const AuBlockBuilder *b, AuBlockBuildStats *s);

/* ---- Compiled files ---- */
typedef struct AuBlockFileInfo {
    uint32_t blocks, allows;
    char title[AU_BLOCK_TITLE_MAX];
} AuBlockFileInfo;
/* Checks a compiled file completely (size, counts, order, checksum).
 * Nothing is trusted before this passes. */
bool au_block_file_check(const uint8_t *data, size_t len, AuBlockFileInfo *info);

/* ---- The set of lists in use ---- */
typedef struct AuBlocklist AuBlocklist;

typedef struct AuBlockStats {
    uint32_t checked;     /* requests examined */
    uint32_t blocked;     /* subresources and script requests refused */
    uint32_t navigations; /* navigations stopped at the interstitial */
    uint32_t first_party; /* listed hosts let through as first-party */
    uint32_t cache_hits;  /* verdicts answered from the per-host cache */
    uint32_t lists;       /* lists attached */
    uint32_t entries;     /* their entries */
    uint32_t bytes;       /* memory the attached lists use */
} AuBlockStats;

AuBlocklist *au_blocklist_new(void);
void au_blocklist_free(AuBlocklist *bl);
/* Blocking as a whole (the preference). Off: nothing is blocked. */
void au_blocklist_set_enabled(AuBlocklist *bl, bool on);
bool au_blocklist_enabled(const AuBlocklist *bl);
/* Attaches a compiled file under id, replacing a list with that id. The
 * set takes the block (released through mem when detached) whether or not
 * the file is valid; false when it is not (nothing is attached, and a
 * list already attached under id stays). */
bool au_blocklist_attach(AuBlocklist *bl, const char *id, uint8_t *data, size_t len, const AuBlockMem *mem);
void au_blocklist_detach(AuBlocklist *bl, const char *id);
bool au_blocklist_has(const AuBlocklist *bl, const char *id, AuBlockFileInfo *info);
/* The user's own entries: domains separated by spaces. allow overrides
 * everything; block is checked like a list; sites_off names registrable
 * domains of sites where nothing is blocked. NULL means none. */
void au_blocklist_set_user(AuBlocklist *bl, const char *allow, const char *block, const char *sites_off);
/* "Visit Anyway": nothing on host's site is blocked as a navigation
 * until the application quits. */
void au_blocklist_allow_session(AuBlocklist *bl, const char *host);

/* Whether the lists cover host (lower case ASCII), ignoring who asks:
 * the personal allow entries, then the personal block entries, then each
 * list's exceptions and entries. *name is the list's title. Verdicts are
 * cached per host. */
bool au_blocklist_listed(AuBlocklist *bl, const char *host, const char **name);

/* The decision for one request. target is the URL about to be fetched;
 * top_level_url is the page it belongs to, or NULL when the request is
 * itself a top-level navigation. A listed host is blocked unless blocking
 * is off, the site (the page's, or the navigation's own) is switched off
 * or allowed for the session, or the request is first-party (the same
 * registrable domain as the page). Writes "blocked: LIST" to reason and
 * counts the request. */
bool au_blocklist_check(AuBlocklist *bl, const AuUrl *target, const char *top_level_url, char *reason, size_t cap);
/* Whether blocking is switched off for the site of url (sites_off). */
bool au_blocklist_site_off(const AuBlocklist *bl, const char *url);
void au_blocklist_stats(const AuBlocklist *bl, AuBlockStats *s);

/* ---- Space-separated domain lists (the preferences) ----
 * Whether list holds domain, and list with domain added or removed
 * (appended to out). */
bool au_block_words_has(const char *list, const char *domain);
void au_block_words_set(const char *list, const char *domain, bool present, AuBuf *out);
/* Cleans what the user typed into a domain: trims, lower-cases, drops a
 * scheme, path and port, converts IDN. False when it is not a domain. */
bool au_block_clean_domain(const char *s, size_t n, AuBuf *out);
/* The registrable domain of the host of url (the host itself when it has
 * none), lower case, into out; false for URLs without a host. */
bool au_block_site_of(const char *url, AuBuf *out);

#endif
