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

/* ---- Non-fatal allocation ----
 * For allocators that must survive running out, such as the JavaScript
 * engine's, which turns a failed allocation into a script out-of-memory
 * error. These return NULL on failure and never call the OOM handler.
 * Free the result with au_free. */
void *au_try_alloc(size_t n);
void *au_try_realloc(void *p, size_t n);

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
/* NUL-terminates (the NUL is not counted in len) and returns data. */
char *au_buf_cstr(AuBuf *b);
/* Returns the NUL-terminated contents and resets the buffer; the caller
 * frees the result with au_free. */
char *au_buf_take(AuBuf *b, size_t *len);

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

/* ---- Atoms ----
 * An atom is an interned string: equal strings give equal atoms, so names
 * compare by integer. Atoms live for the life of the program. 0 is "no
 * atom". Interning is case sensitive; lowercase HTML names before
 * interning.
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

#endif
