/* Domain blocklists (see blocklist.h). */
#include "blocklist.h"
#include "psl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB AU_BLOCK_HASH_BYTES
#define CHUNK_ENTRIES 16384u
#define MAX_ALLOW 200000u
#define MAX_PROBES 10

/* ---- Memory ---- */

static void *mem_alloc(const AuBlockMem *m, size_t n)
{
    return m->alloc ? m->alloc(m->ctx, n) : au_try_alloc(n);
}

static void mem_release(const AuBlockMem *m, void *p)
{
    if (!p)
        return;
    if (m->alloc)
        m->release(m->ctx, p);
    else
        au_free(p);
}

/* ---- Hash ---- */

uint64_t au_block_hash(const char *s, size_t n)
{
    /* FNV-1a 64 over the bytes, then MurmurHash3's finalizer so every
     * input bit reaches the 48 bits kept. */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h >> 16;
}

static void put_hash(uint8_t *p, uint64_t h)
{
    p[0] = (uint8_t)(h >> 40);
    p[1] = (uint8_t)(h >> 32);
    p[2] = (uint8_t)(h >> 24);
    p[3] = (uint8_t)(h >> 16);
    p[4] = (uint8_t)(h >> 8);
    p[5] = (uint8_t)h;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int cmp_entry(const void *a, const void *b)
{
    return memcmp(a, b, HB);
}

static uint32_t fnv32(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Whether the sorted entries hold key. */
static bool find_entry(const uint8_t *entries, uint32_t n, const uint8_t *key)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = memcmp(entries + (size_t)mid * HB, key, HB);
        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* ---- Names ---- */

/* Validates and lower-cases an ASCII domain into out (at least 254
 * bytes). Returns its length, or 0 when it is not one: two labels at
 * least, each 1 to 63 of [a-z0-9_-], the last not a number. */
static size_t ascii_domain(const char *s, size_t n, char *out)
{
    if (n && s[n - 1] == '.')
        n--;
    if (n < 3 || n > 253)
        return 0;
    size_t label = 0, dots = 0, last = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        if (c == '.') {
            if (!label)
                return 0;
            label = 0;
            dots++;
            last = i + 1;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            if (++label > 63)
                return 0;
        } else {
            return 0;
        }
        out[i] = (char)c;
    }
    if (!label || !dots)
        return 0;
    bool digits = true;
    for (size_t i = last; i < n; i++)
        if (out[i] < '0' || out[i] > '9')
            digits = false;
    if (digits)
        return 0;
    out[n] = 0;
    return n;
}

/* The same for any name: non-ASCII names go through the URL parser's
 * host conversion (Punycode). */
static size_t any_domain(const char *s, size_t n, char *out)
{
    bool ascii = true;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] >= 0x80)
            ascii = false;
    if (ascii)
        return ascii_domain(s, n, out);
    if (n > 1024)
        return 0;
    AuBuf b;
    au_buf_init(&b);
    AuUrlHostType type = AU_URL_HOST_NULL;
    size_t r = 0;
    if (au_url_host_parse(s, n, true, &b, &type) && type == AU_URL_HOST_DOMAIN)
        r = ascii_domain(b.data, b.len, out);
    au_buf_free(&b);
    return r;
}

/* ---- Builder ---- */

struct AuBlockBuilder {
    AuBlockMem mem;
    uint8_t **chunks; /* sorted, CHUNK_ENTRIES each but the last */
    uint32_t *chunk_n;
    size_t nchunks, capchunks;
    uint8_t *cur;
    uint32_t cur_n;
    uint32_t total;
    uint8_t *allow;
    uint32_t nallow, capallow;
    char line[AU_BLOCK_LINE_MAX];
    size_t line_len;
    bool line_over;
    char title[AU_BLOCK_TITLE_MAX];
    bool title_fixed;
    AuBlockBuildStats st;
    bool failed;
    /* finishing */
    int phase; /* 0 feeding, 1 merging, 2 done */
    uint8_t *out;
    uint32_t out_n;
    uint32_t *heap, *pos;
    size_t nheap;
    uint8_t last[HB];
    bool have_last;
};

AuBlockBuilder *au_block_builder_new(const AuBlockMem *mem)
{
    AuBlockBuilder *b = au_calloc(sizeof *b);
    if (mem)
        b->mem = *mem;
    return b;
}

void au_block_builder_free(AuBlockBuilder *b)
{
    if (!b)
        return;
    for (size_t i = 0; i < b->nchunks; i++)
        mem_release(&b->mem, b->chunks[i]);
    mem_release(&b->mem, b->cur);
    mem_release(&b->mem, b->out);
    au_free(b->chunks);
    au_free(b->chunk_n);
    au_free(b->allow);
    au_free(b->heap);
    au_free(b->pos);
    au_free(b);
}

void au_block_builder_set_title(AuBlockBuilder *b, const char *title)
{
    snprintf(b->title, sizeof b->title, "%s", title ? title : "");
    b->title_fixed = title && title[0];
}

void au_block_builder_stats(const AuBlockBuilder *b, AuBlockBuildStats *s)
{
    *s = b->st;
}

/* Sorts the chunk being filled and moves it to the list. */
static bool close_chunk(AuBlockBuilder *b)
{
    if (!b->cur)
        return true;
    if (!b->cur_n) {
        mem_release(&b->mem, b->cur);
        b->cur = NULL;
        return true;
    }
    if (b->nchunks == b->capchunks) {
        size_t cap = b->capchunks ? b->capchunks * 2 : 16;
        uint8_t **c = au_try_realloc(b->chunks, cap * sizeof *c);
        if (!c)
            return false;
        b->chunks = c;
        uint32_t *cn = au_try_realloc(b->chunk_n, cap * sizeof *cn);
        if (!cn)
            return false;
        b->chunk_n = cn;
        b->capchunks = cap;
    }
    qsort(b->cur, b->cur_n, HB, cmp_entry);
    b->chunks[b->nchunks] = b->cur;
    b->chunk_n[b->nchunks++] = b->cur_n;
    b->cur = NULL;
    b->cur_n = 0;
    return true;
}

bool au_block_builder_add_hash(AuBlockBuilder *b, uint64_t hash, bool allow)
{
    if (b->failed || b->phase)
        return false;
    if (allow) {
        if (b->nallow == b->capallow) {
            uint32_t cap = b->capallow ? b->capallow * 2 : 256;
            uint8_t *a = b->nallow >= MAX_ALLOW ? NULL : au_try_realloc(b->allow, (size_t)cap * HB);
            if (!a) {
                b->failed = true;
                return false;
            }
            b->allow = a;
            b->capallow = cap;
        }
        put_hash(b->allow + (size_t)b->nallow++ * HB, hash);
        return true;
    }
    if (b->total >= AU_BLOCK_MAX_ENTRIES) {
        b->failed = true;
        return false;
    }
    if (!b->cur) {
        b->cur = mem_alloc(&b->mem, (size_t)CHUNK_ENTRIES * HB);
        if (!b->cur) {
            b->failed = true;
            return false;
        }
        b->cur_n = 0;
    }
    put_hash(b->cur + (size_t)b->cur_n++ * HB, hash);
    b->total++;
    if (b->cur_n == CHUNK_ENTRIES && !close_chunk(b)) {
        b->failed = true;
        return false;
    }
    return true;
}

bool au_block_builder_add(AuBlockBuilder *b, const char *domain, size_t n, bool allow)
{
    char name[256];
    size_t len = any_domain(domain, n, name);
    if (!len)
        return false;
    if (!au_block_builder_add_hash(b, au_block_hash(name, len), allow))
        return false;
    b->st.accepted++;
    if (allow)
        b->st.allowed++;
    return true;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

/* A hosts file's address column: an IPv4 or IPv6 literal. */
static bool is_address(const char *s, size_t n)
{
    bool dot = false, colon = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.')
            dot = true;
        else if (c == ':')
            colon = true;
        else if (!au_is_hex(c))
            return false;
    }
    if (colon)
        return true;
    if (!dot)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '.' && !au_is_digit(s[i]))
            return false;
    return true;
}

static bool starts(const char *s, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    return n >= l && !memcmp(s, lit, l);
}

/* One line of a list, without its line break. */
static void take_line(AuBlockBuilder *b, const char *s, size_t n)
{
    while (n && (is_blank(s[0]) || (unsigned char)s[0] < 0x20)) {
        s++;
        n--;
    }
    while (n && (is_blank(s[n - 1]) || (unsigned char)s[n - 1] < 0x20))
        n--;
    if (!n)
        return;
    if (s[0] == '#' || s[0] == '!') {
        /* "# Title: NAME" names the list. */
        s++;
        n--;
        while (n && is_blank(s[0])) {
            s++;
            n--;
        }
        if (!b->title_fixed && !b->title[0] && starts(s, n, "Title:")) {
            s += 6;
            n -= 6;
            while (n && is_blank(s[0])) {
                s++;
                n--;
            }
            size_t k = 0;
            for (size_t i = 0; i < n && k + 1 < sizeof b->title; i++) {
                unsigned char c = (unsigned char)s[i];
                b->title[k++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
            }
            b->title[k] = 0;
        }
        return;
    }
    if (s[0] == '[')
        return;
    bool allow = false;
    if (starts(s, n, "@@")) {
        allow = true;
        s += 2;
        n -= 2;
    }
    if (starts(s, n, "||")) {
        s += 2;
        n -= 2;
        const char *hat = memchr(s, '^', n);
        if (hat) {
            size_t rest = n - (size_t)(hat - s) - 1;
            const char *r = hat + 1;
            if (!rest || (rest == 1 && r[0] == '|') || (rest == 10 && !memcmp(r, "$important", 10))) {
                if (au_block_builder_add(b, s, (size_t)(hat - s), allow))
                    return;
            }
        }
        b->st.ignored++;
        return;
    }
    if (allow) {
        b->st.ignored++;
        return;
    }
    if (starts(s, n, "local=/") || starts(s, n, "address=/") || starts(s, n, "server=/")) {
        const char *a = memchr(s, '/', n);
        a++;
        const char *e = memchr(a, '/', n - (size_t)(a - s));
        if (e && au_block_builder_add(b, a, (size_t)(e - a), false))
            return;
        b->st.ignored++;
        return;
    }
    /* Tokens: a domain alone, or an address followed by names. */
    size_t i = 0, ntok = 0;
    bool hosts = false, took = false;
    while (i < n) {
        while (i < n && is_blank(s[i]))
            i++;
        if (i >= n || s[i] == '#')
            break;
        size_t st = i;
        while (i < n && !is_blank(s[i]))
            i++;
        const char *t = s + st;
        size_t tl = i - st;
        if (ntok == 0) {
            if (is_address(t, tl)) {
                hosts = true;
            } else {
                /* A plain or wildcard domain must be alone on its line. */
                size_t j = i;
                while (j < n && is_blank(s[j]))
                    j++;
                if (j < n && s[j] != '#')
                    break;
                if (tl > 2 && t[0] == '*' && t[1] == '.') {
                    t += 2;
                    tl -= 2;
                }
                took = au_block_builder_add(b, t, tl, false);
                break;
            }
        } else if (hosts) {
            if (!(tl == 21 && !memcmp(t, "localhost.localdomain", 21)) && au_block_builder_add(b, t, tl, false))
                took = true;
        }
        ntok++;
        if (ntok > 16)
            break;
    }
    if (!took)
        b->st.ignored++;
}

bool au_block_builder_feed(AuBlockBuilder *b, const void *bytes, size_t len)
{
    const char *p = bytes;
    if (b->phase)
        return false;
    for (size_t i = 0; i < len && !b->failed; i++) {
        char c = p[i];
        if (c == '\n' || c == '\r') {
            if (b->line_over)
                b->st.too_long++;
            else if (b->line_len)
                take_line(b, b->line, b->line_len);
            if (b->line_len || b->line_over)
                b->st.lines++;
            b->line_len = 0;
            b->line_over = false;
        } else if (b->line_len < sizeof b->line) {
            b->line[b->line_len++] = c;
        } else {
            b->line_over = true;
        }
    }
    return !b->failed;
}

/* The merge keeps the chunks in a binary heap ordered by each chunk's
 * next entry. */
static const uint8_t *heap_entry(const AuBlockBuilder *b, size_t i)
{
    uint32_t c = b->heap[i];
    return b->chunks[c] + (size_t)b->pos[c] * HB;
}

static void sift_down(AuBlockBuilder *b, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < b->nheap && memcmp(heap_entry(b, l), heap_entry(b, m), HB) < 0)
            m = l;
        if (r < b->nheap && memcmp(heap_entry(b, r), heap_entry(b, m), HB) < 0)
            m = r;
        if (m == i)
            return;
        uint32_t t = b->heap[i];
        b->heap[i] = b->heap[m];
        b->heap[m] = t;
        i = m;
    }
}

int au_block_builder_finish(AuBlockBuilder *b, uint32_t budget)
{
    if (b->failed)
        return -1;
    if (b->phase == 2)
        return 1;
    if (b->phase == 0) {
        /* A last line without a line break. */
        if (b->line_len && !b->line_over) {
            take_line(b, b->line, b->line_len);
            b->st.lines++;
        }
        b->line_len = 0;
        if (b->failed || !close_chunk(b)) {
            b->failed = true;
            return -1;
        }
        if (b->nallow) {
            qsort(b->allow, b->nallow, HB, cmp_entry);
            uint32_t k = 1;
            for (uint32_t i = 1; i < b->nallow; i++)
                if (memcmp(b->allow + (size_t)i * HB, b->allow + (size_t)(k - 1) * HB, HB))
                    memmove(b->allow + (size_t)k++ * HB, b->allow + (size_t)i * HB, HB);
            b->nallow = k;
        }
        b->out = mem_alloc(&b->mem, AU_BLOCK_HEADER + ((size_t)b->total + b->nallow) * HB);
        b->heap = au_try_alloc((b->nchunks + 1) * sizeof *b->heap);
        b->pos = au_try_alloc((b->nchunks + 1) * sizeof *b->pos);
        if (!b->out || !b->heap || !b->pos) {
            b->failed = true;
            return -1;
        }
        for (size_t i = 0; i < b->nchunks; i++) {
            b->heap[i] = (uint32_t)i;
            b->pos[i] = 0;
        }
        b->nheap = b->nchunks;
        for (size_t i = b->nheap / 2; i-- > 0;)
            sift_down(b, i);
        b->phase = 1;
    }
    uint32_t done = 0;
    while (b->nheap) {
        if (budget && done >= budget)
            return 0;
        uint32_t c = b->heap[0];
        const uint8_t *e = b->chunks[c] + (size_t)b->pos[c] * HB;
        if (!b->have_last || memcmp(e, b->last, HB)) {
            memcpy(b->out + AU_BLOCK_HEADER + (size_t)b->out_n++ * HB, e, HB);
            memcpy(b->last, e, HB);
            b->have_last = true;
        }
        done++;
        if (++b->pos[c] == b->chunk_n[c]) {
            mem_release(&b->mem, b->chunks[c]);
            b->chunks[c] = NULL;
            b->heap[0] = b->heap[--b->nheap];
        }
        if (b->nheap)
            sift_down(b, 0);
    }
    uint8_t *o = b->out;
    uint8_t *entries = o + AU_BLOCK_HEADER;
    if (b->nallow)
        memcpy(entries + (size_t)b->out_n * HB, b->allow, (size_t)b->nallow * HB);
    memset(o, 0, AU_BLOCK_HEADER);
    memcpy(o, "AuBL", 4);
    o[5] = 1;
    o[7] = HB;
    wr32(o + 8, b->out_n);
    wr32(o + 12, b->nallow);
    wr32(o + 16, fnv32(2166136261u, entries, ((size_t)b->out_n + b->nallow) * HB));
    memcpy(o + 24, b->title, strlen(b->title));
    b->phase = 2;
    return 1;
}

bool au_block_builder_take(AuBlockBuilder *b, uint8_t **data, size_t *len)
{
    if (b->phase != 2 || !b->out)
        return false;
    *data = b->out;
    *len = AU_BLOCK_HEADER + ((size_t)b->out_n + b->nallow) * HB;
    b->out = NULL;
    return true;
}

/* ---- Compiled files ---- */

bool au_block_file_check(const uint8_t *d, size_t len, AuBlockFileInfo *info)
{
    if (!d || len < AU_BLOCK_HEADER || memcmp(d, "AuBL", 4) || d[4] != 0 || d[5] != 1 || d[6] != 0 || d[7] != HB)
        return false;
    uint32_t nb = rd32(d + 8), na = rd32(d + 12);
    if (nb > AU_BLOCK_MAX_ENTRIES || na > MAX_ALLOW)
        return false;
    if (len != AU_BLOCK_HEADER + ((size_t)nb + na) * HB)
        return false;
    if (d[AU_BLOCK_HEADER - 1] != 0)
        return false;
    const uint8_t *e = d + AU_BLOCK_HEADER;
    if (fnv32(2166136261u, e, ((size_t)nb + na) * HB) != rd32(d + 16))
        return false;
    for (uint32_t i = 1; i < nb; i++)
        if (memcmp(e + (size_t)(i - 1) * HB, e + (size_t)i * HB, HB) >= 0)
            return false;
    const uint8_t *a = e + (size_t)nb * HB;
    for (uint32_t i = 1; i < na; i++)
        if (memcmp(a + (size_t)(i - 1) * HB, a + (size_t)i * HB, HB) >= 0)
            return false;
    if (info) {
        info->blocks = nb;
        info->allows = na;
        memcpy(info->title, d + 24, AU_BLOCK_TITLE_MAX);
        info->title[AU_BLOCK_TITLE_MAX - 1] = 0;
        for (char *p = info->title; *p; p++)
            if ((unsigned char)*p < 0x20)
                *p = '?';
    }
    return true;
}

/* ---- Word lists ---- */

bool au_block_words_has(const char *list, const char *domain)
{
    size_t n = strlen(domain);
    if (!list || !n)
        return false;
    for (const char *p = list; *p;) {
        while (*p == ' ')
            p++;
        size_t l = strcspn(p, " ");
        if (l == n && !memcmp(p, domain, n))
            return true;
        p += l;
    }
    return false;
}

void au_block_words_set(const char *list, const char *domain, bool present, AuBuf *out)
{
    size_t n = strlen(domain);
    bool first = true;
    for (const char *p = list ? list : ""; *p;) {
        while (*p == ' ')
            p++;
        size_t l = strcspn(p, " ");
        if (l && !(l == n && !memcmp(p, domain, n))) {
            if (!first)
                au_buf_putc(out, ' ');
            au_buf_append(out, p, l);
            first = false;
        }
        p += l;
    }
    if (present && n) {
        if (!first)
            au_buf_putc(out, ' ');
        au_buf_append(out, domain, n);
    }
}

bool au_block_clean_domain(const char *s, size_t n, AuBuf *out)
{
    while (n && (unsigned char)s[0] <= ' ') {
        s++;
        n--;
    }
    while (n && (unsigned char)s[n - 1] <= ' ')
        n--;
    for (size_t i = 0; i + 2 < n; i++)
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            s += i + 3;
            n -= i + 3;
            break;
        }
    for (size_t i = 0; i < n; i++)
        if (s[i] == '/' || s[i] == ':' || s[i] == '?' || s[i] == '#') {
            n = i;
            break;
        }
    if (n > 2 && s[0] == '*' && s[1] == '.') {
        s += 2;
        n -= 2;
    }
    char name[256];
    size_t len = any_domain(s, n, name);
    if (!len)
        return false;
    au_buf_append(out, name, len);
    return true;
}

/* The registrable domain of host (the host itself without one). */
static void site_of_host(const char *host, const char **site, size_t *len)
{
    if (!au_psl_registrable(host, site, len)) {
        *site = host;
        *len = strlen(host);
    }
}

bool au_block_site_of(const char *url, AuBuf *out)
{
    AuUrl u;
    if (!url || !au_url_parse(&u, url, NULL))
        return false;
    bool ok = u.host[0] != 0;
    if (ok) {
        const char *s;
        size_t n;
        site_of_host(u.host, &s, &n);
        au_buf_append(out, s, n);
    }
    au_url_free(&u);
    return ok;
}

/* ---- The set ---- */

typedef struct List {
    char id[32];
    uint8_t *data;
    size_t len;
    AuBlockMem mem;
    uint32_t nb, na;
    char title[AU_BLOCK_TITLE_MAX];
} List;

#define CACHE_SLOTS 512
#define V_NONE 1
#define V_USER 2
#define V_LIST 3 /* + list index */

typedef struct Verdict {
    uint8_t key[HB];
    uint8_t v; /* 0: empty */
} Verdict;

struct AuBlocklist {
    bool enabled;
    List lists[AU_BLOCK_MAX_LISTS];
    int nlists;
    char *allow, *block, *sites_off;
    char *session; /* sites, separated by spaces */
    Verdict cache[CACHE_SLOTS];
    /* The last page asked about, and its site. */
    char *top_url;
    char top_site[256];
    AuBlockStats st;
};

AuBlocklist *au_blocklist_new(void)
{
    AuBlocklist *bl = au_calloc(sizeof *bl);
    bl->enabled = true;
    return bl;
}

static void list_drop(List *l)
{
    mem_release(&l->mem, l->data);
    memset(l, 0, sizeof *l);
}

void au_blocklist_free(AuBlocklist *bl)
{
    if (!bl)
        return;
    for (int i = 0; i < bl->nlists; i++)
        list_drop(&bl->lists[i]);
    au_free(bl->allow);
    au_free(bl->block);
    au_free(bl->sites_off);
    au_free(bl->session);
    au_free(bl->top_url);
    au_free(bl);
}

void au_blocklist_set_enabled(AuBlocklist *bl, bool on) { bl->enabled = on; }
bool au_blocklist_enabled(const AuBlocklist *bl) { return bl->enabled; }

static void forget_verdicts(AuBlocklist *bl)
{
    memset(bl->cache, 0, sizeof bl->cache);
}

static int find_list(const AuBlocklist *bl, const char *id)
{
    for (int i = 0; i < bl->nlists; i++)
        if (!strcmp(bl->lists[i].id, id))
            return i;
    return -1;
}

bool au_blocklist_attach(AuBlocklist *bl, const char *id, uint8_t *data, size_t len, const AuBlockMem *mem)
{
    AuBlockMem m = {NULL, NULL, NULL};
    if (mem)
        m = *mem;
    AuBlockFileInfo info;
    int at = find_list(bl, id);
    if (!au_block_file_check(data, len, &info) || strlen(id) >= sizeof bl->lists[0].id ||
        (at < 0 && bl->nlists == AU_BLOCK_MAX_LISTS)) {
        mem_release(&m, data);
        return false;
    }
    if (at >= 0)
        mem_release(&bl->lists[at].mem, bl->lists[at].data);
    else
        at = bl->nlists++;
    List *l = &bl->lists[at];
    memset(l, 0, sizeof *l);
    snprintf(l->id, sizeof l->id, "%s", id);
    l->data = data;
    l->len = len;
    l->mem = m;
    l->nb = info.blocks;
    l->na = info.allows;
    snprintf(l->title, sizeof l->title, "%s", info.title[0] ? info.title : id);
    forget_verdicts(bl);
    return true;
}

void au_blocklist_detach(AuBlocklist *bl, const char *id)
{
    int at = find_list(bl, id);
    if (at < 0)
        return;
    list_drop(&bl->lists[at]);
    for (int i = at; i + 1 < bl->nlists; i++)
        bl->lists[i] = bl->lists[i + 1];
    bl->nlists--;
    memset(&bl->lists[bl->nlists], 0, sizeof bl->lists[0]);
    forget_verdicts(bl);
}

bool au_blocklist_has(const AuBlocklist *bl, const char *id, AuBlockFileInfo *info)
{
    int at = find_list(bl, id);
    if (at < 0)
        return false;
    if (info) {
        info->blocks = bl->lists[at].nb;
        info->allows = bl->lists[at].na;
        snprintf(info->title, sizeof info->title, "%s", bl->lists[at].title);
    }
    return true;
}

static void set_words(char **field, const char *v)
{
    au_free(*field);
    *field = NULL;
    if (!v || !v[0])
        return;
    size_t n = strlen(v);
    char *s = au_strndup(v, n);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        s[i] = (c <= ' ' || c == ',') ? ' ' : (char)au_lower(c);
    }
    *field = s;
}

void au_blocklist_set_user(AuBlocklist *bl, const char *allow, const char *block, const char *sites_off)
{
    set_words(&bl->allow, allow);
    set_words(&bl->block, block);
    set_words(&bl->sites_off, sites_off);
    forget_verdicts(bl);
}

void au_blocklist_allow_session(AuBlocklist *bl, const char *host)
{
    const char *s;
    size_t n;
    char low[256];
    size_t hl = strlen(host);
    if (!hl || hl >= sizeof low)
        return;
    for (size_t i = 0; i <= hl; i++)
        low[i] = (char)au_lower((unsigned char)host[i]);
    site_of_host(low, &s, &n);
    char site[256];
    memcpy(site, s, n);
    site[n] = 0;
    AuBuf b;
    au_buf_init(&b);
    au_block_words_set(bl->session, site, true, &b);
    au_free(bl->session);
    bl->session = au_buf_take(&b, NULL);
}

bool au_blocklist_listed(AuBlocklist *bl, const char *host, const char **name)
{
    static const char personal[] = "Personal blocklist";
    size_t n = strlen(host);
    if (name)
        *name = "";
    if (n < 3 || n > 253)
        return false;
    if (!bl->nlists && !bl->block)
        return false;
    uint8_t key[HB];
    put_hash(key, au_block_hash(host, n));
    Verdict *slot = &bl->cache[(key[4] << 8 | key[5]) % CACHE_SLOTS];
    int v = 0;
    if (slot->v && !memcmp(slot->key, key, HB)) {
        v = slot->v;
        bl->st.cache_hits++;
    } else {
        /* The candidates: host and each parent down to the registrable
         * domain. A host without one (a public suffix, an address, a
         * single label) is never listed. */
        const char *dom;
        size_t dl;
        v = V_NONE;
        if (au_psl_registrable(host, &dom, &dl)) {
            const char *cand[MAX_PROBES];
            int nc = 0;
            for (const char *p = host;;) {
                if (nc == MAX_PROBES) {
                    /* Very deep names: keep the candidates nearest the
                     * registrable domain. */
                    memmove(cand, cand + 1, (MAX_PROBES - 1) * sizeof cand[0]);
                    nc--;
                }
                cand[nc++] = p;
                if (p >= dom)
                    break;
                const char *dot = strchr(p, '.');
                if (!dot)
                    break;
                p = dot + 1;
            }
            bool user_allow = false, user_block = false, list_allow = false;
            int list_block = -1;
            for (int i = 0; i < nc; i++) {
                if (bl->allow && au_block_words_has(bl->allow, cand[i]))
                    user_allow = true;
                if (bl->block && au_block_words_has(bl->block, cand[i]))
                    user_block = true;
                if (!bl->nlists)
                    continue;
                uint8_t k[HB];
                put_hash(k, au_block_hash(cand[i], strlen(cand[i])));
                for (int j = 0; j < bl->nlists; j++) {
                    const List *l = &bl->lists[j];
                    const uint8_t *e = l->data + AU_BLOCK_HEADER;
                    if (l->na && find_entry(e + (size_t)l->nb * HB, l->na, k))
                        list_allow = true;
                    if ((list_block < 0 || j < list_block) && find_entry(e, l->nb, k))
                        list_block = j;
                }
            }
            if (user_allow)
                v = V_NONE;
            else if (user_block)
                v = V_USER;
            else if (list_block >= 0 && !list_allow)
                v = V_LIST + list_block;
        }
        memcpy(slot->key, key, HB);
        slot->v = (uint8_t)v;
    }
    if (v == V_NONE)
        return false;
    if (name)
        *name = v == V_USER ? personal : bl->lists[v - V_LIST].title;
    return true;
}

static const char *top_site(AuBlocklist *bl, const char *top_level_url)
{
    if (bl->top_url && !strcmp(bl->top_url, top_level_url))
        return bl->top_site;
    au_free(bl->top_url);
    bl->top_url = au_strndup(top_level_url, strlen(top_level_url));
    bl->top_site[0] = 0;
    AuBuf b;
    au_buf_init(&b);
    if (au_block_site_of(top_level_url, &b) && b.len < sizeof bl->top_site) {
        memcpy(bl->top_site, b.data, b.len);
        bl->top_site[b.len] = 0;
    }
    au_buf_free(&b);
    return bl->top_site;
}

bool au_blocklist_site_off(const AuBlocklist *bl, const char *url)
{
    if (!bl->sites_off)
        return false;
    AuBuf b;
    au_buf_init(&b);
    bool off = au_block_site_of(url, &b) && au_block_words_has(bl->sites_off, au_buf_cstr(&b));
    au_buf_free(&b);
    return off;
}

bool au_blocklist_check(AuBlocklist *bl, const AuUrl *target, const char *top_level_url, char *reason, size_t cap)
{
    if (reason && cap)
        reason[0] = 0;
    if (!bl->enabled || target->host_type != AU_URL_HOST_DOMAIN || (!bl->nlists && !bl->block))
        return false;
    const char *host = target->host;
    bl->st.checked++;
    const char *site;
    size_t sl;
    site_of_host(host, &site, &sl);
    char own[256];
    if (sl >= sizeof own)
        return false;
    memcpy(own, site, sl);
    own[sl] = 0;
    const char *name;
    if (top_level_url) {
        const char *ts = top_site(bl, top_level_url);
        if (ts[0] && !strcmp(ts, own)) {
            if (au_blocklist_listed(bl, host, &name))
                bl->st.first_party++;
            return false;
        }
        if (ts[0] && bl->sites_off && au_block_words_has(bl->sites_off, ts))
            return false;
    } else {
        if (bl->sites_off && au_block_words_has(bl->sites_off, own))
            return false;
        if (bl->session && au_block_words_has(bl->session, own))
            return false;
    }
    if (!au_blocklist_listed(bl, host, &name))
        return false;
    if (top_level_url)
        bl->st.blocked++;
    else
        bl->st.navigations++;
    if (reason && cap)
        snprintf(reason, cap, "blocked: %s", name);
    return true;
}

void au_blocklist_stats(const AuBlocklist *bl, AuBlockStats *s)
{
    *s = bl->st;
    s->lists = (uint32_t)bl->nlists;
    s->entries = 0;
    s->bytes = 0;
    for (int i = 0; i < bl->nlists; i++) {
        s->entries += bl->lists[i].nb;
        s->bytes += (uint32_t)bl->lists[i].len;
    }
}
