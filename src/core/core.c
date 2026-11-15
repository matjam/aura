#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Allocation ---- */

static void default_oom(size_t wanted)
{
    fprintf(stderr, "aura: out of memory (%lu bytes)\n", (unsigned long)wanted);
    abort();
}

static void (*oom_handler)(size_t) = default_oom;

void au_set_oom_handler(void (*handler)(size_t wanted))
{
    oom_handler = handler ? handler : default_oom;
}

static void *raw_alloc(size_t n)
{
    return au_heap_enabled() ? au_heap_alloc(n) : malloc(n);
}

static void *raw_realloc(void *p, size_t n)
{
    return au_heap_enabled() ? au_heap_realloc(p, n) : realloc(p, n);
}

static bool (*release_handler)(size_t);
static bool releasing;

void au_set_release_handler(bool (*release)(size_t wanted))
{
    release_handler = release;
}

/* Asks the application to free its caches; true when it freed something
 * (worth trying the allocation again). */
static bool release(size_t n)
{
    if (!release_handler || releasing)
        return false;
    releasing = true;
    bool freed = release_handler(n);
    releasing = false;
    return freed;
}

void *au_alloc(size_t n)
{
    void *p = raw_alloc(n ? n : 1);
    while (!p && release(n))
        p = raw_alloc(n ? n : 1);
    if (!p) {
        oom_handler(n);
        abort(); /* the handler must not return */
    }
    return p;
}

void *au_calloc(size_t n)
{
    void *p = au_alloc(n);
    memset(p, 0, n);
    return p;
}

void *au_realloc(void *p, size_t n)
{
    void *q;
    /* A failed realloc leaves p valid, so trying again with p is right. */
    do
        q = raw_realloc(p, n ? n : 1);
    while (!q && release(n));
    if (!q) {
        oom_handler(n);
        abort();
    }
    return q;
}

void au_free(void *p)
{
    if (au_heap_enabled())
        au_heap_free(p);
    else
        free(p);
}

/* ---- Non-fatal allocation ---- */

void *au_try_alloc(size_t n)
{
    return raw_alloc(n ? n : 1);
}

void *au_try_realloc(void *p, size_t n)
{
    return raw_realloc(p, n ? n : 1);
}

char *au_strndup(const char *s, size_t n)
{
    char *p = au_alloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- Buffer ---- */

void au_buf_init(AuBuf *b)
{
    b->data = NULL;
    b->len = b->cap = 0;
}

void au_buf_free(AuBuf *b)
{
    au_free(b->data);
    au_buf_init(b);
}

void au_buf_clear(AuBuf *b)
{
    b->len = 0;
}

void au_buf_reserve(AuBuf *b, size_t extra)
{
    size_t need = b->len + extra + 1; /* +1 keeps room for au_buf_cstr's NUL */
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 32;
    while (cap < need)
        cap *= 2;
    b->data = au_realloc(b->data, cap);
    b->cap = cap;
}

void au_buf_append(AuBuf *b, const void *p, size_t n)
{
    if (!n)
        return;
    au_buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void au_buf_puts(AuBuf *b, const char *s)
{
    au_buf_append(b, s, strlen(s));
}

void au_buf_putc(AuBuf *b, char c)
{
    au_buf_reserve(b, 1);
    b->data[b->len++] = c;
}

void au_buf_put_utf8(AuBuf *b, uint32_t cp)
{
    char tmp[4];
    au_buf_append(b, tmp, (size_t)au_utf8_encode(cp, tmp));
}

void au_buf_printf(AuBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    au_buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void au_put_i64(AuBuf *b, int64_t v)
{
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        au_buf_putc(b, '-');
    while (n)
        au_buf_putc(b, tmp[--n]);
}

char *au_buf_cstr(AuBuf *b)
{
    au_buf_reserve(b, 0);
    b->data[b->len] = 0;
    return b->data;
}

char *au_buf_take(AuBuf *b, size_t *len)
{
    char *p = au_buf_cstr(b);
    if (len)
        *len = b->len;
    au_buf_init(b);
    return p;
}

/* ---- UTF-8 ---- */

uint32_t au_utf8_decode(const char *s, size_t n, size_t *adv)
{
    const unsigned char *u = (const unsigned char *)s;
    uint32_t c = u[0];
    size_t need;
    uint32_t min;

    *adv = 1;
    if (c < 0x80)
        return c;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1; min = 0x80; c &= 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2; min = 0x800; c &= 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3; min = 0x10000; c &= 0x07;
    } else {
        return 0xFFFD;
    }
    if (n < need + 1)
        return 0xFFFD;
    for (size_t i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80)
            return 0xFFFD;
        c = (c << 6) | (u[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0xFFFD;
    *adv = need + 1;
    return c;
}

int au_utf8_encode(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool au_ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (au_lower((unsigned char)a[i]) != au_lower((unsigned char)b[i]))
            return false;
    return true;
}

/* ---- Atoms ----
 * strs[] maps atom to string; table[] is an open-addressed hash of atoms. */

static const char *const static_atoms[] = {
    "",
#define ATOM(id, str) str,
#include "atoms_html.def"
#include "atoms_css.def"
#undef ATOM
};

typedef struct {
    const char *s;
    uint32_t len, hash;
} AtomEntry;

static AtomEntry *atom_entries; /* indexed by atom */
static uint32_t atom_count, atom_cap;
static AuAtom *atom_table;
static uint32_t atom_table_size; /* power of two */

static uint32_t hash_bytes(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void table_insert(AuAtom a)
{
    uint32_t mask = atom_table_size - 1;
    uint32_t i = atom_entries[a].hash & mask;
    while (atom_table[i])
        i = (i + 1) & mask;
    atom_table[i] = a;
}

static void table_grow(void)
{
    au_free(atom_table);
    atom_table_size = atom_table_size ? atom_table_size * 2 : 1024;
    atom_table = au_calloc(atom_table_size * sizeof *atom_table);
    for (AuAtom a = 1; a < atom_count; a++)
        table_insert(a);
}

static AuAtom add_entry(const char *s, uint32_t len, uint32_t hash)
{
    if (atom_count == atom_cap) {
        atom_cap = atom_cap ? atom_cap * 2 : 512;
        atom_entries = au_realloc(atom_entries, atom_cap * sizeof *atom_entries);
    }
    AuAtom a = atom_count++;
    atom_entries[a].s = s;
    atom_entries[a].len = len;
    atom_entries[a].hash = hash;
    if (a) {
        if ((atom_count + 1) * 2 > atom_table_size)
            table_grow();
        else
            table_insert(a);
    }
    return a;
}

static void atoms_init(void)
{
    for (uint32_t i = 0; i < AU_ATOM_STATIC_COUNT; i++) {
        uint32_t len = (uint32_t)strlen(static_atoms[i]);
        add_entry(static_atoms[i], len, hash_bytes(static_atoms[i], len));
    }
}

static AuAtom find(const char *s, size_t n, uint32_t h)
{
    uint32_t mask = atom_table_size - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        AuAtom a = atom_table[i];
        if (!a)
            return 0;
        const AtomEntry *e = &atom_entries[a];
        if (e->hash == h && e->len == n && memcmp(e->s, s, n) == 0)
            return a;
    }
}

AuAtom au_atom_lookup(const char *s, size_t n)
{
    if (!atom_count)
        atoms_init();
    if (!n)
        return 0;
    return find(s, n, hash_bytes(s, n));
}

AuAtom au_atom(const char *s, size_t n)
{
    if (!atom_count)
        atoms_init();
    if (!n)
        return 0;
    uint32_t h = hash_bytes(s, n);
    AuAtom a = find(s, n, h);
    if (a)
        return a;
    return add_entry(au_strndup(s, n), (uint32_t)n, h);
}

AuAtom au_atom_cstr(const char *s)
{
    return au_atom(s, strlen(s));
}

const char *au_atom_str(AuAtom a)
{
    if (!atom_count)
        atoms_init();
    return a < atom_count ? atom_entries[a].s : "";
}

size_t au_atom_len(AuAtom a)
{
    if (!atom_count)
        atoms_init();
    return a < atom_count ? atom_entries[a].len : 0;
}
