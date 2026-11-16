#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The definitions below are the functions themselves, not core.h's
 * macros that record the site and the zone. */
#undef au_alloc
#undef au_calloc
#undef au_realloc
#undef au_try_alloc
#undef au_try_realloc
#undef au_strndup
#undef au_buf_reserve
#undef au_buf_append
#undef au_buf_puts
#undef au_buf_putc
#undef au_buf_put_utf8
#undef au_buf_printf
#undef au_buf_cstr

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

static bool (*low_memory_check)(size_t);

void au_set_low_memory_check(bool (*low)(size_t n))
{
    low_memory_check = low;
}

bool au_memory_low(size_t n)
{
    return low_memory_check && low_memory_check(n);
}

void au_set_release_handler(bool (*release)(size_t wanted))
{
    release_handler = release;
}

/* Gives the pooled allocator's empty slabs back to the platform; true when
 * there were any. */
static bool trim(void)
{
    return au_heap_enabled() && au_heap_trim() > 0;
}

/* Frees what can be freed; true when it freed something (worth trying the
 * allocation again). First the pool's empty slabs, which cost nothing to
 * give back; then the application's caches, and the slabs they emptied. */
static bool release(size_t n)
{
    if (releasing)
        return false;
    if (trim())
        return true;
    if (!release_handler)
        return false;
    releasing = true;
    bool freed = release_handler(n);
    releasing = false;
    trim();
    return freed;
}

static void *base_alloc(size_t n)
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

static void *base_realloc(void *p, size_t n)
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

static void base_free(void *p)
{
    if (au_heap_enabled())
        au_heap_free(p);
    else
        free(p);
}

/* ---- Non-fatal allocation ---- */

/* These free no caches, but give back the pool's empty slabs. */
static void *base_try_alloc(size_t n)
{
    void *p = raw_alloc(n ? n : 1);
    if (!p && trim())
        p = raw_alloc(n ? n : 1);
    return p;
}

static void *base_try_realloc(void *p, size_t n)
{
    void *q = raw_realloc(p, n ? n : 1);
    if (!q && trim())
        q = raw_realloc(p, n ? n : 1);
    return q;
}


#ifndef AU_MEM_TRACK

void *au_alloc(size_t n) { return base_alloc(n); }
void *au_realloc(void *p, size_t n) { return base_realloc(p, n); }
void au_free(void *p) { base_free(p); }
void *au_try_alloc(size_t n) { return base_try_alloc(n); }
void *au_try_realloc(void *p, size_t n) { return base_try_realloc(p, n); }

#else

/* ---- Allocation counter (AU_MEM_TRACK) ----
 * Every block carries a header with its size and the call site that
 * allocated it; core.h's macros record the site (the source file) before
 * each call. A block keeps the site that first allocated it. */
const char *au_mem_site_;

typedef struct MemHdr {
    size_t size;
    uint32_t site;
    uint32_t pad;
} MemHdr;

#define MEM_SITES 4096
static struct MemSite {
    const char *file;
    size_t live, peak, blocks, at_peak;
} mem_sites[MEM_SITES];
static int mem_nsites;
static size_t mem_live, mem_peak;

static uint32_t mem_site(void)
{
    const char *f = au_mem_site_ ? au_mem_site_ : "src/core/core.c";
    au_mem_site_ = NULL;
    static const char *last_f;
    static uint32_t last_i;
    if (f == last_f)
        return last_i;
    for (int i = 0; i < mem_nsites; i++)
        if (mem_sites[i].file == f || !strcmp(mem_sites[i].file, f)) {
            last_f = f;
            last_i = (uint32_t)i;
            return last_i;
        }
    if (mem_nsites == MEM_SITES)
        return MEM_SITES - 1;
    mem_sites[mem_nsites].file = f;
    return (uint32_t)mem_nsites++;
}

static void mem_add(MemHdr *h)
{
    struct MemSite *s = &mem_sites[h->site];
    s->live += h->size;
    s->blocks++;
    if (s->live > s->peak)
        s->peak = s->live;
    mem_live += h->size;
    if (mem_live > mem_peak) {
        mem_peak = mem_live;
        if ((mem_peak & ~(size_t)0xFFFF) != ((mem_peak - h->size) & ~(size_t)0xFFFF))
            for (int i = 0; i < mem_nsites; i++)
                mem_sites[i].at_peak = mem_sites[i].live;
    }
}

static void mem_sub(MemHdr *h)
{
    mem_sites[h->site].live -= h->size;
    mem_sites[h->site].blocks--;
    mem_live -= h->size;
}

static void *mem_new(void *raw, size_t n)
{
    MemHdr *h = raw;
    if (!h) {
        au_mem_site_ = NULL;
        return NULL;
    }
    h->size = n;
    h->site = mem_site();
    mem_add(h);
    return h + 1;
}

static void *mem_moved(void *raw, size_t n, uint32_t site)
{
    MemHdr *h = raw;
    au_mem_site_ = NULL;
    h->size = n;
    h->site = site;
    mem_add(h);
    return h + 1;
}

void *au_alloc(size_t n) { return mem_new(base_alloc(n + sizeof(MemHdr)), n); }
void *au_try_alloc(size_t n) { return mem_new(base_try_alloc(n + sizeof(MemHdr)), n); }

void *au_realloc(void *p, size_t n)
{
    if (!p)
        return au_alloc(n);
    MemHdr *h = (MemHdr *)p - 1;
    uint32_t site = h->site;
    mem_sub(h);
    return mem_moved(base_realloc(h, n + sizeof(MemHdr)), n, site);
}

void *au_try_realloc(void *p, size_t n)
{
    if (!p)
        return au_try_alloc(n);
    MemHdr *h = (MemHdr *)p - 1;
    MemHdr old = *h;
    void *q = base_try_realloc(h, n + sizeof(MemHdr));
    if (!q) {
        au_mem_site_ = NULL;
        return NULL;
    }
    mem_sub(&old);
    return mem_moved(q, n, old.site);
}

void au_free(void *p)
{
    if (!p)
        return;
    MemHdr *h = (MemHdr *)p - 1;
    mem_sub(h);
    base_free(h);
}

const char *au_mem_site_of(const void *raw)
{
    const MemHdr *h = raw;
    return h->site < (uint32_t)mem_nsites ? mem_sites[h->site].file : "?";
}

size_t au_mem_live(void) { return mem_live; }
size_t au_mem_peak(void) { return mem_peak; }

int au_mem_sites(AuMemSite *out, int max)
{
    int n = 0;
    for (int i = 0; i < mem_nsites && n < max; i++) {
        out[n].file = mem_sites[i].file;
        out[n].live = mem_sites[i].live;
        out[n].peak = mem_sites[i].peak;
        out[n].at_peak = mem_sites[i].at_peak;
        out[n].blocks = mem_sites[i].blocks;
        n++;
    }
    return n;
}

#endif

void *au_calloc(size_t n)
{
    void *p = au_alloc(n);
    memset(p, 0, n);
    return p;
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

/* An entry's holders: a bit per live scope (au_atom_scope_new), and
 * ATOM_PERM for an atom that lives for the life of the program. A free
 * entry has s NULL and holders 0, and hash links it to the next free one. */
typedef struct {
    const char *s;
    uint32_t len, hash;
    uint32_t holders;
} AtomEntry;

#define ATOM_PERM 0x80000000u
#define ATOM_SCOPES 31

static AtomEntry *atom_entries; /* indexed by atom */
static uint32_t atom_count, atom_cap;
static AuAtom *atom_table;
static uint32_t atom_table_size; /* power of two */
static uint32_t atom_free_head;  /* first free entry, 0 for none */
static uint32_t atom_live;       /* entries in use, the static ones included */
static size_t atom_string_bytes;
static uint32_t atom_generation;
/* What an atom interned now is held by, and the set that collects it. */
static uint32_t atom_bits = ATOM_PERM;
static AuAtomSet *atom_set;
static uint32_t scopes_live; /* a bit per live scope */

static void set_push(AuAtomSet *set, AuAtom a);

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

/* The atom table and array live as long as the program (docs/memory.md,
 * "Zones"); the caller's zone is set back after. */
static void table_grow(void)
{
    unsigned zone = au_heap_zone_set(AU_HEAP_ZONE_LONG);
    au_free(atom_table);
    atom_table_size = atom_table_size ? atom_table_size * 2 : 1024;
    atom_table = au_calloc(atom_table_size * sizeof *atom_table);
    for (AuAtom a = 1; a < atom_count; a++)
        if (atom_entries[a].s)
            table_insert(a);
    au_heap_zone_set(zone);
}

/* The table again, after entries were freed: as small as the live
 * entries allow (at most half full), and without the freed ones. */
static void table_rebuild(void)
{
    uint32_t size = 1024;
    while ((atom_live + 1) * 2 > size && size < 0x40000000u)
        size *= 2;
    unsigned zone = au_heap_zone_set(AU_HEAP_ZONE_LONG);
    au_free(atom_table);
    atom_table_size = size;
    atom_table = au_calloc(atom_table_size * sizeof *atom_table);
    for (AuAtom a = 1; a < atom_count; a++)
        if (atom_entries[a].s)
            table_insert(a);
    au_heap_zone_set(zone);
}

static AuAtom add_entry(const char *s, uint32_t len, uint32_t hash, uint32_t holders)
{
    AuAtom a;
    if (atom_free_head) {
        a = atom_free_head;
        atom_free_head = atom_entries[a].hash;
    } else {
        if (atom_count == atom_cap) {
            unsigned zone = au_heap_zone_set(AU_HEAP_ZONE_LONG);
            atom_cap = atom_cap ? atom_cap * 2 : 512;
            atom_entries = au_realloc(atom_entries, atom_cap * sizeof *atom_entries);
            au_heap_zone_set(zone);
        }
        a = atom_count++;
    }
    atom_entries[a].s = s;
    atom_entries[a].len = len;
    atom_entries[a].hash = hash;
    atom_entries[a].holders = holders;
    atom_live++;
    if (a) {
        if ((atom_live + 1) * 2 > atom_table_size)
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
        add_entry(static_atoms[i], len, hash_bytes(static_atoms[i], len), ATOM_PERM);
    }
}

/* An atom interned or looked up now: it gets the current holders, and
 * the collecting set records it unless it lives for good. */
static uint32_t cur_bits(void)
{
    /* a scope freed while it was entered holds nothing any more */
    uint32_t b = atom_bits & (scopes_live | ATOM_PERM);
    return b ? b : ATOM_PERM;
}

static void atom_used(AuAtom a)
{
    AtomEntry *e = &atom_entries[a];
    e->holders |= cur_bits();
    if (atom_set && !(e->holders & ATOM_PERM))
        set_push(atom_set, a);
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
    AuAtom a = find(s, n, hash_bytes(s, n));
    if (a)
        atom_used(a);
    return a;
}

AuAtom au_atom(const char *s, size_t n)
{
    if (!atom_count)
        atoms_init();
    if (!n)
        return 0;
    uint32_t h = hash_bytes(s, n);
    AuAtom a = find(s, n, h);
    if (a) {
        atom_used(a);
        return a;
    }
    atom_string_bytes += n + 1;
    uint32_t bits = cur_bits();
    /* Atom texts are long-lived: a page's atom becomes permanent when
     * code outside the page looks it up, and its text cannot move then
     * (docs/memory.md, "Zones"). */
    unsigned zone = au_heap_zone_set(AU_HEAP_ZONE_LONG);
    a = add_entry(au_strndup(s, n), (uint32_t)n, h, bits);
    au_heap_zone_set(zone);
    if (atom_set && !(bits & ATOM_PERM))
        set_push(atom_set, a);
    return a;
}

AuAtom au_atom_cstr(const char *s)
{
    return au_atom(s, strlen(s));
}

AuAtom au_atom_permanent(const char *s, size_t n)
{
    AuAtomBits prev = au_atom_scope_enter(0);
    AuAtom a = au_atom(s, n);
    au_atom_scope_leave(prev);
    return a;
}

AuAtom au_atom_peek(const char *s, size_t n)
{
    if (!atom_count)
        atoms_init();
    return n ? find(s, n, hash_bytes(s, n)) : 0;
}

void au_atom_use(AuAtom a)
{
    if (a && a < atom_count && atom_entries[a].s)
        atom_used(a);
}

const char *au_atom_str(AuAtom a)
{
    if (!atom_count)
        atoms_init();
    return a < atom_count && atom_entries[a].s ? atom_entries[a].s : "";
}

size_t au_atom_len(AuAtom a)
{
    if (!atom_count)
        atoms_init();
    return a < atom_count ? atom_entries[a].len : 0;
}

/* ---- Atom scopes ----
 * A scope is a bit in each atom's holders. The page layer gives each
 * top-level document a scope and enters it while it works on the
 * document; every atom interned or looked up meanwhile gets its bit.
 * Freeing the scope clears the bit and frees the atoms nothing else holds.
 * Code that runs in no scope (atom_bits ATOM_PERM) makes the atoms it
 * touches permanent, so a missed entry costs memory, never an atom freed
 * while in use. */

AuAtomScope au_atom_scope_new(void)
{
    if (!atom_count)
        atoms_init();
    for (uint32_t i = 0; i < ATOM_SCOPES; i++)
        if (!(scopes_live & (1u << i))) {
            scopes_live |= 1u << i;
            return i + 1;
        }
    return 0; /* none left: the document's atoms are permanent */
}

static uint32_t scope_bit(AuAtomScope s) { return s && s <= ATOM_SCOPES ? 1u << (s - 1) : 0; }

static void scope_release(AuAtomScope s, AuAtomScope heir, bool has_heir);

/* After atoms were freed: the free entries at the end of the array are
 * dropped, the array shrinks when it is less than half used, and the
 * free list is made again lowest number first, so that new atoms take
 * low numbers and the end of the array stays free to drop next time. */
static void entries_trim(void)
{
    uint32_t n = atom_count;
    while (n > AU_ATOM_STATIC_COUNT && !atom_entries[n - 1].s)
        n--;
    atom_count = n;
    uint32_t cap = 512;
    while (cap < n)
        cap *= 2;
    if (cap < atom_cap / 2) {
        unsigned zone = au_heap_zone_set(AU_HEAP_ZONE_LONG);
        atom_entries = au_realloc(atom_entries, cap * sizeof *atom_entries);
        au_heap_zone_set(zone);
        atom_cap = cap;
    }
    atom_free_head = 0;
    for (uint32_t a = n; a-- > AU_ATOM_STATIC_COUNT;)
        if (!atom_entries[a].s) {
            atom_entries[a].hash = atom_free_head;
            atom_free_head = a;
        }
}

void au_atom_scope_free(AuAtomScope s) { scope_release(s, 0, false); }

void au_atom_scope_free_into(AuAtomScope s, AuAtomScope heir) { scope_release(s, heir, true); }

/* Clears s from every atom, giving heir its place (the program when heir
 * is 0 or not live), and frees the atoms nothing holds then. */
static void scope_release(AuAtomScope s, AuAtomScope heir, bool has_heir)
{
    uint32_t bit = scope_bit(s);
    if (!bit || !(scopes_live & bit))
        return;
    scopes_live &= ~bit;
    uint32_t to = 0;
    if (has_heir) {
        to = scope_bit(heir) & scopes_live;
        if (!to)
            to = ATOM_PERM;
    }
    uint32_t freed = 0;
    for (AuAtom a = AU_ATOM_STATIC_COUNT; a < atom_count; a++) {
        AtomEntry *e = &atom_entries[a];
        if (!e->s || !(e->holders & bit))
            continue;
        e->holders = (e->holders & ~bit) | to;
        if (e->holders)
            continue;
        atom_string_bytes -= e->len + 1;
        au_free((char *)e->s);
        e->s = NULL;
        e->len = 0;
        e->hash = atom_free_head;
        atom_free_head = a;
        atom_live--;
        freed++;
    }
    if (freed) {
        entries_trim();
        table_rebuild();
        atom_generation++;
    }
}

AuAtomBits au_atom_scope_enter(AuAtomScope s)
{
    AuAtomBits prev = atom_bits;
    uint32_t bit = scope_bit(s);
    atom_bits = bit && (scopes_live & bit) ? bit : ATOM_PERM;
    return prev;
}

void au_atom_scope_leave(AuAtomBits prev) { atom_bits = prev; }

AuAtomBits au_atom_scope_bits(void) { return cur_bits(); }

AuAtomBits au_atom_scope_live(void) { return scopes_live | ATOM_PERM; }

/* ---- Atom sets ----
 * A set is a sorted array of atoms (the ones that do not live for good)
 * that some structure refers to, so that another holder can take them all
 * (au_atom_set_hold). While a set collects, atoms are appended unsorted
 * after the sorted part; collect_end sorts and merges them. */

static void set_push(AuAtomSet *set, AuAtom a)
{
    if (set->n > set->sorted && set->a[set->n - 1] == a)
        return;
    if (set->n == set->cap) {
        set->cap = set->cap ? set->cap * 2 : 16;
        set->a = au_realloc(set->a, set->cap * sizeof *set->a);
    }
    set->a[set->n++] = a;
}

static int cmp_atom(const void *x, const void *y)
{
    AuAtom a = *(const AuAtom *)x, b = *(const AuAtom *)y;
    return a < b ? -1 : a > b;
}

static bool set_has_sorted(const AuAtomSet *set, AuAtom a)
{
    uint32_t lo = 0, hi = set->sorted;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (set->a[mid] == a)
            return true;
        if (set->a[mid] < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

static void set_normalize(AuAtomSet *set)
{
    if (set->n == set->sorted)
        return;
    AuAtom *tail = set->a + set->sorted;
    uint32_t nt = set->n - set->sorted;
    qsort(tail, nt, sizeof *tail, cmp_atom);
    /* the tail's atoms, once each, that the sorted part lacks */
    uint32_t m = 0;
    for (uint32_t i = 0; i < nt; i++)
        if ((!m || tail[m - 1] != tail[i]) && !set_has_sorted(set, tail[i]))
            tail[m++] = tail[i];
    if (m) {
        AuAtom *merged = au_alloc((set->sorted + m) * sizeof *merged);
        uint32_t i = 0, j = 0, k = 0;
        while (i < set->sorted || j < m)
            merged[k++] = j >= m || (i < set->sorted && set->a[i] < tail[j]) ? set->a[i++] : tail[j++];
        au_free(set->a);
        set->a = merged;
        set->cap = k;
        set->n = set->sorted = k;
    } else {
        set->n = set->sorted;
    }
}

AuAtomCollect au_atom_collect_begin(AuAtomSet *set, AuAtomBits bits)
{
    AuAtomCollect prev = {atom_set, atom_bits};
    atom_set = set;
    atom_bits = bits ? bits : ATOM_PERM;
    return prev;
}

void au_atom_collect_end(AuAtomCollect prev)
{
    if (atom_set)
        set_normalize(atom_set);
    atom_set = prev.set;
    atom_bits = prev.bits;
}

void au_atom_set_hold(const AuAtomSet *set, AuAtomBits bits)
{
    bits &= scopes_live | ATOM_PERM;
    if (!bits)
        bits = ATOM_PERM;
    for (uint32_t i = 0; i < set->n; i++) {
        AuAtom a = set->a[i];
        if (a < atom_count && atom_entries[a].s)
            atom_entries[a].holders |= bits;
    }
}

void au_atom_set_free(AuAtomSet *set)
{
    au_free(set->a);
    memset(set, 0, sizeof *set);
}

void au_atom_stats(AuAtomStats *st)
{
    if (!atom_count)
        atoms_init();
    memset(st, 0, sizeof *st);
    st->atoms = atom_live;
    st->dynamic = atom_live - AU_ATOM_STATIC_COUNT;
    st->string_bytes = atom_string_bytes;
    st->table_bytes = (size_t)atom_cap * sizeof(AtomEntry) + (size_t)atom_table_size * sizeof(AuAtom);
    for (AuAtom a = AU_ATOM_STATIC_COUNT; a < atom_count; a++)
        if (atom_entries[a].s && (atom_entries[a].holders & ATOM_PERM))
            st->permanent++;
    st->scopes = 0;
    for (uint32_t i = 0; i < ATOM_SCOPES; i++)
        if (scopes_live & (1u << i))
            st->scopes++;
}

uint32_t au_atom_generation(void) { return atom_generation; }
