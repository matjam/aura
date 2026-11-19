/* Aura core: allocation, byte buffers, UTF-8 and atoms.
 * Portable C99. Nothing here calls the OS. */
#ifndef AURA_CORE_H
#define AURA_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Allocation ----
 * These never return NULL. When memory runs out they call the handler set
 * with au_set_oom_handler (default: abort). The Mac OS 9 shell installs a
 * handler that unwinds to the event loop. */
void *au_alloc(size_t n);
void *au_calloc(size_t n);
void *au_realloc(void *p, size_t n);
void au_free(void *p);
char *au_strndup(const char *s, size_t n);
void au_set_oom_handler(void (*handler)(size_t wanted));
/* Low memory: when malloc fails, au_alloc and au_realloc first call the
 * release handler (none by default), which frees what the application
 * can do without (caches) and returns true if it freed anything; the
 * allocation is then tried again, and the OOM handler runs only when the
 * release handler has nothing left to free. The handler is not called
 * again while it runs. */
void au_set_release_handler(bool (*release)(size_t wanted));

/* Whether the platform is too short of memory to give n more bytes to a
 * user that can do without (the JavaScript engine: it then fails the
 * script with an out-of-memory error, while there is still memory for the
 * page itself). The platform installs the check; without one the answer
 * is always no. */
void au_set_low_memory_check(bool (*low)(size_t n));
bool au_memory_low(size_t n);

/* ---- Non-fatal allocation ----
 * For allocators that must survive running out, such as the JavaScript
 * engine's, which turns a failed allocation into a script out-of-memory
 * error. These return NULL on failure and never call the OOM handler.
 * Free the result with au_free. */
void *au_try_alloc(size_t n);
void *au_try_realloc(void *p, size_t n);

/* ---- Allocation counter ----
 * To see which source files hold memory, build with -DAU_MEM_TRACK (the
 * CMake option AURA_MEM_TRACK). Every allocating call then records its
 * source file and line, and each block has a 16-byte header. au_mem_sites
 * lists the bytes live per site, their peak, and what each held when the total
 * was last at its peak (to 64 KB). A buffer is counted for the file that
 * first allocated it. */
#ifdef AU_MEM_TRACK
typedef struct AuMemSite {
    const char *file;
    size_t live, peak, at_peak, blocks;
} AuMemSite;
extern const char *au_mem_site_;
#define AU_MEM_STR2_(x) #x
#define AU_MEM_STR_(x) AU_MEM_STR2_(x)
#define AU_MEM_HERE_ __FILE__ ":" AU_MEM_STR_(__LINE__)
size_t au_mem_live(void);
size_t au_mem_peak(void);
/* The site that allocated a block, given the block as au_heap_alloc
 * returned it (the counter's header comes first). */
const char *au_mem_site_of(const void *raw);
int au_mem_sites(AuMemSite *out, int max);
#endif

/* ---- Zones ----
 * Every block belongs to one of two zones: the page zone, for what a page
 * makes and frees with it (documents, styles, layout, script heaps), and
 * the long-lived zone, for what outlives pages (the network stack and the
 * HTTP cache, TLS connections, fonts, atoms, the browser's lists and the
 * platform shell). The pooled allocator never mixes the two in a slab and
 * the Mac OS 9 shell keeps the long-lived zone in a region of its own, so
 * a closed page leaves its memory in one piece (docs/memory.md, "Zones").
 *
 * A source file's zone is AU_HEAP_ZONE, which the build defines per file
 * (CMakeLists.txt, AURA_LONG_LIVED_SOURCES; the page zone otherwise). Each
 * allocating call below sets the zone to it first (au_heap_zone_set),
 * and the zone stays until the next one sets it. Files compiled
 * with AU_HEAP_ZONE_INHERIT (src/core, src/codec, src/url: helpers that allocate
 * for their callers) leave it alone, so their blocks take the zone of the
 * caller's last allocating call. au_heap_zone_set sets it for code that
 * must choose otherwise (the atom table), and returns the previous one.
 *
 * - To allocate one long-lived block in a page-zone file (a cache kept in
 *   a static variable), use au_alloc_long, au_calloc_long or
 *   au_realloc_long.
 * - To make everything a call allocates long-lived, whatever the files it
 *   reaches (decoding a favicon the browser keeps), hold the zone:
 *   int h = au_heap_zone_hold(AU_HEAP_ZONE_LONG); ...;
 *   au_heap_zone_unhold(h). */
#define AU_HEAP_ZONE_PAGE 0u
#define AU_HEAP_ZONE_LONG 1u
unsigned au_heap_zone_set(unsigned zone);
int au_heap_zone_hold(unsigned zone);
void au_heap_zone_unhold(int prev);
#ifdef AU_HEAP_ZONE_INHERIT
#define AU_ZONE_PRE_ ((void)0)
#else
#ifndef AU_HEAP_ZONE
#define AU_HEAP_ZONE AU_HEAP_ZONE_PAGE
#endif
/* A call, not an assignment: two allocations in one expression then set
 * it in some order rather than without a sequence point. */
#define AU_ZONE_PRE_ ((void)au_heap_zone_set(AU_HEAP_ZONE))
#endif
#ifdef AU_MEM_TRACK
#define AU_ALLOC_PRE_ (au_mem_site_ = AU_MEM_HERE_, AU_ZONE_PRE_)
#else
#define AU_ALLOC_PRE_ AU_ZONE_PRE_
#endif
#define au_alloc(n) (AU_ALLOC_PRE_, au_alloc(n))
#define au_calloc(n) (AU_ALLOC_PRE_, au_calloc(n))
#define au_realloc(p, n) (AU_ALLOC_PRE_, au_realloc(p, n))
#define au_try_alloc(n) (AU_ALLOC_PRE_, au_try_alloc(n))
#define au_try_realloc(p, n) (AU_ALLOC_PRE_, au_try_realloc(p, n))
#define au_strndup(s, n) (AU_ALLOC_PRE_, au_strndup(s, n))
#define AU_LONG_PRE_ (AU_ALLOC_PRE_, (void)au_heap_zone_set(AU_HEAP_ZONE_LONG))
#define au_alloc_long(n) (AU_LONG_PRE_, (au_alloc)(n))
#define au_calloc_long(n) (AU_LONG_PRE_, (au_calloc)(n))
#define au_realloc_long(p, n) (AU_LONG_PRE_, (au_realloc)(p, n))

/* ---- The pooled allocator (heap.c) ----
 * To make au_alloc and the functions above take blocks of up to 16 KB
 * from slabs instead of calling malloc for each, call au_heap_enable
 * before the first allocation, with the functions that get and release
 * platform memory (get returns NULL when there is none). The Mac OS 9
 * shell does: there malloc is one NewPtr per block, and the Memory Manager
 * is slow with the tens of thousands of blocks a page holds. Blocks
 * allocated before the call must not be freed after it.
 *
 * au_heap_trim returns the empty slabs the pool keeps for reuse to the
 * platform and says how many bytes went; au_alloc and au_try_alloc call it
 * when the platform has no memory, before the release handler. */
/* get's how: the zone (AU_HEAP_ZONE_PAGE or AU_HEAP_ZONE_LONG, the low
 * bits), and AU_HEAP_GET_SLAB when the block will be a slab, else it is
 * one large block. */
#define AU_HEAP_GET_SLAB 0x10u
#define AU_HEAP_ZONE_MASK 0x0Fu
void au_heap_enable(void *(*get)(size_t n, unsigned how), void (*release)(void *p));
bool au_heap_enabled(void);
/* Optional: resize makes a large platform block (one get returned without
 * AU_HEAP_GET_SLAB) n bytes long where it is, never moving it, and
 * returns false when it cannot. au_realloc of a block over 16 KB tries it
 * before it allocates, copies and frees. */
void au_heap_set_resize(bool (*resize)(void *p, size_t n));
void *au_heap_alloc(size_t n); /* NULL when the platform has no memory */
void *au_heap_realloc(void *p, size_t n);
void au_heap_free(void *p);
size_t au_heap_trim(void);
typedef struct AuHeapStats {
    size_t blocks, used_bytes;  /* in use: blocks and the bytes asked for */
    size_t slabs, large;        /* held from the platform: slabs, and blocks too large for one */
    size_t system_bytes;        /* bytes held from the platform */
    size_t mid_slabs, mid_blocks; /* of those: slabs of blocks over 2 KB, and their blocks in use */
    size_t empty_bytes;         /* bytes of empty slabs kept for reuse (au_heap_trim returns them) */
    size_t long_bytes;          /* of system_bytes: the long-lived zone's */
    size_t resized;             /* large blocks au_realloc resized in place (au_heap_set_resize) */
} AuHeapStats;
/* The zone of a block from au_heap_alloc. */
unsigned au_heap_zone_of(const void *p);
void au_heap_stats(AuHeapStats *s);
/* For a map of the platform's blocks: calls fn for each block in use in
 * pb, a block that get returned with the given how (a slab's blocks, or
 * the one large block). */
void au_heap_walk_platform_block(void *pb, unsigned how, void (*fn)(void *ctx, void *block, size_t n), void *ctx);

/* ---- Growable byte buffer ---- */
typedef struct AuBuf {
    char *data;
    size_t len, cap;
} AuBuf;

void au_buf_init(AuBuf *b);
void au_buf_free(AuBuf *b);
void au_buf_clear(AuBuf *b);                 /* len = 0, keeps storage */
void au_buf_reserve(AuBuf *b, size_t extra); /* room for extra more bytes */
void au_buf_append(AuBuf *b, const void *p, size_t n);
void au_buf_puts(AuBuf *b, const char *s);
void au_buf_putc(AuBuf *b, char c);
void au_buf_put_utf8(AuBuf *b, uint32_t cp);
void au_buf_printf(AuBuf *b, const char *fmt, ...);
/* Appends v in decimal. Use this for 64-bit values instead of printf's
 * %lld: the newlib that Retro68 links for Mac OS 9 does not implement it,
 * and prints nothing useful. */
void au_put_i64(AuBuf *b, int64_t v);
/* NUL-terminates (the NUL is not counted in len) and returns data. */
char *au_buf_cstr(AuBuf *b);
/* Returns the NUL-terminated contents and resets the buffer; the caller
 * frees the result with au_free. */
char *au_buf_take(AuBuf *b, size_t *len);
#define au_buf_reserve(b, n) (AU_ALLOC_PRE_, au_buf_reserve(b, n))
#define au_buf_append(b, p, n) (AU_ALLOC_PRE_, au_buf_append(b, p, n))
#define au_buf_puts(b, s) (AU_ALLOC_PRE_, au_buf_puts(b, s))
#define au_buf_putc(b, c) (AU_ALLOC_PRE_, au_buf_putc(b, c))
#define au_buf_put_utf8(b, c) (AU_ALLOC_PRE_, au_buf_put_utf8(b, c))
#define au_buf_printf(...) (AU_ALLOC_PRE_, au_buf_printf(__VA_ARGS__))
#define au_buf_cstr(b) (AU_ALLOC_PRE_, au_buf_cstr(b))

/* ---- UTF-8 ----
 * au_utf8_decode reads one code point from s (n bytes available, n > 0),
 * stores how many bytes it used in *adv (at least 1) and returns the code
 * point, or 0xFFFD for a malformed sequence. */
uint32_t au_utf8_decode(const char *s, size_t n, size_t *adv);
/* Writes cp to out (up to 4 bytes) and returns the byte count. */
int au_utf8_encode(uint32_t cp, char out[4]);

/* ---- ASCII helpers (locale independent) ---- */
static inline bool au_is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static inline bool au_is_digit(int c) { return c >= '0' && c <= '9'; }
static inline bool au_is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline bool au_is_hex(int c) { return au_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int au_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
bool au_ieq(const char *a, size_t alen, const char *b, size_t blen); /* ASCII case-insensitive */

/* ---- stack use ----
 * Mac OS 9 gives Aura a small stack (see main.c), and layout calls itself
 * once per level of nested boxes. A function with large locals that the
 * compiler inlines adds them to its caller's frame for the whole call:
 * AU_NOINLINE keeps such a function out of a caller that recurses. */
#if defined(__GNUC__)
#define AU_NOINLINE __attribute__((noinline))
#else
#define AU_NOINLINE
#endif

/* ---- float to int ----
 * A cast of a float outside int's range (or NaN) is undefined, and pages
 * reach every size and offset with values such as 1e38px. au_f2i
 * saturates at the ends of int's range; au_f2px saturates at plus and
 * minus 2^29, for pixel coordinates, so that the difference of two
 * results and a result plus a small pad still fit an int. NaN gives 0. */
static inline int au_f2i(float v)
{
    if (v >= 2147483520.0f)
        return 2147483520;
    if (v <= -2147483648.0f)
        return (int)(-2147483647 - 1);
    return v != v ? 0 : (int)v;
}
static inline int au_f2px(float v)
{
    if (v >= 536870912.0f)
        return 536870912;
    if (v <= -536870912.0f)
        return -536870912;
    return v != v ? 0 : (int)v;
}

/* ---- Atoms ----
 * An atom is an interned string: equal strings give equal atoms, so names
 * compare by integer. 0 is "no atom". Interning is case sensitive;
 * lowercase HTML names before interning.
 *
 * An atom lives while a holder holds it: a scope (one per top-level
 * document, au_atom_scope_new), or the program itself. Every atom interned
 * or looked up gets the scope entered at that moment (au_atom_scope_enter)
 * as a holder; with no scope entered it lives for good. au_atom_scope_free
 * frees the atoms only that scope held, and their numbers are used again.
 * So a structure that outlives the scope it was built in (a cache shared
 * by all documents, say) must intern its atoms with no scope entered
 * (au_atom_permanent), or take them for its holders (au_atom_set_hold),
 * and a cache that returns atoms it keeps calls au_atom_use on each and
 * drops what it keeps when au_atom_generation changes.
 *
 * Names known at compile time are listed in src/core/atoms_*.def as
 * ATOM(identifier, "string") and get the constant AU_ATOM_identifier. */
typedef uint32_t AuAtom;

enum {
    AU_ATOM_NONE = 0,
#define ATOM(id, str) AU_ATOM_##id,
#include "atoms_html.def"
#include "atoms_css.def"
#undef ATOM
    AU_ATOM_STATIC_COUNT
};

AuAtom au_atom(const char *s, size_t n);
AuAtom au_atom_cstr(const char *s);
/* Returns 0 if the string was never interned; does not intern it. */
AuAtom au_atom_lookup(const char *s, size_t n);
const char *au_atom_str(AuAtom a); /* NUL-terminated; "" for 0 */
size_t au_atom_len(AuAtom a);
/* Interns s with no scope entered: the atom lives for good. */
AuAtom au_atom_permanent(const char *s, size_t n);
/* Gives a the holders an atom interned now would get. */
void au_atom_use(AuAtom a);
/* au_atom_lookup without giving the atom a holder: for tests and
 * reports, never for an atom that is kept. */
AuAtom au_atom_peek(const char *s, size_t n);

/* Scopes. A scope is 1 to 31; 0 means none (atoms live for good), which
 * au_atom_scope_new returns when 31 scopes are live. enter returns what
 * was entered before, for leave. */
typedef uint32_t AuAtomScope;
typedef uint32_t AuAtomBits; /* a set of holders */
AuAtomScope au_atom_scope_new(void);
void au_atom_scope_free(AuAtomScope s);
/* The same, giving heir the atoms s held (the program when heir is 0). */
void au_atom_scope_free_into(AuAtomScope s, AuAtomScope heir);
AuAtomBits au_atom_scope_enter(AuAtomScope s);
void au_atom_scope_leave(AuAtomBits prev);
AuAtomBits au_atom_scope_bits(void); /* the holders an atom interned now gets */
AuAtomBits au_atom_scope_live(void); /* every live scope, and the program */

/* An atom set: the atoms (other than those that live for good) that a
 * structure refers to, so that a later holder of the structure can hold
 * them too (a style sheet shared by documents of several scopes). While a
 * set collects, every atom interned or looked up is added to it and gets
 * the holders bits instead of the entered scope's. Zero-initialize a set. */
typedef struct AuAtomSet {
    AuAtom *a;
    uint32_t n, sorted, cap;
} AuAtomSet;
typedef struct AuAtomCollect {
    AuAtomSet *set;
    AuAtomBits bits;
} AuAtomCollect;
AuAtomCollect au_atom_collect_begin(AuAtomSet *set, AuAtomBits bits);
void au_atom_collect_end(AuAtomCollect prev);
void au_atom_set_hold(const AuAtomSet *set, AuAtomBits bits);
void au_atom_set_free(AuAtomSet *set);

typedef struct AuAtomStats {
    uint32_t atoms;      /* live, the compile-time ones included */
    uint32_t dynamic;    /* live, interned at run time */
    uint32_t permanent;  /* of those, the ones that live for good */
    uint32_t scopes;     /* live scopes */
    size_t string_bytes; /* the dynamic atoms' strings */
    size_t table_bytes;  /* the entry array and the hash table */
} AuAtomStats;
void au_atom_stats(AuAtomStats *st);
/* Changes each time a scope's atoms are freed (their numbers may then be
 * given to other strings). */
uint32_t au_atom_generation(void);

/* The platform this copy of Aura runs on, as the part in parentheses of
 * a user agent ("Macintosh; ARM Mac OS X 15_6"). A port sets it at start
 * from the real system (au_set_ua_platform), before the preferences are
 * read; until then, and in the host tools, it is AU_UA_PLATFORM
 * (core/uaplatform.h). The string is copied, up to 95 bytes. */
const char *au_ua_platform(void);
void au_set_ua_platform(const char *part);

#endif
