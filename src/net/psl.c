/* Public Suffix List lookup over the front-coded table in psl_data.c. */
#include "psl.h"

#include <string.h>

/* Compares the reversed-label key with the entry text e (length n). */
static int cmp_bytes(const char *a, size_t an, const uint8_t *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    int c = memcmp(a, b, n);
    if (c)
        return c;
    return an < bn ? -1 : an > bn ? 1 : 0;
}

/* Looks up the reversed key; returns its flags, or 0 when absent. */
static int lookup(const char *key, size_t kn)
{
    if (!au_psl_nblocks)
        return 0;
    /* Binary search over the blocks' first entries (stored in full). */
    uint32_t lo = 0, hi = au_psl_nblocks;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = au_psl_data + au_psl_blocks[mid];
        if (cmp_bytes(key, kn, e + 3, e[2]) < 0)
            hi = mid;
        else
            lo = mid;
    }
    /* Decode the block. */
    uint8_t cur[256];
    size_t curn = 0;
    uint32_t pos = au_psl_blocks[lo];
    uint32_t end = lo + 1 < au_psl_nblocks ? au_psl_blocks[lo + 1] : au_psl_data_len;
    while (pos + 3 <= end) {
        uint8_t flags = au_psl_data[pos], shared = au_psl_data[pos + 1], sl = au_psl_data[pos + 2];
        if (shared > curn || pos + 3u + sl > end || (size_t)shared + sl > sizeof cur)
            return 0; /* damaged table */
        memcpy(cur + shared, au_psl_data + pos + 3, sl);
        curn = (size_t)shared + sl;
        pos += 3u + sl;
        int c = cmp_bytes(key, kn, cur, curn);
        if (c == 0)
            return flags;
        if (c < 0)
            return 0;
    }
    return 0;
}

size_t au_psl_suffix_len(const char *host, size_t n)
{
    if (n == 0 || n > 253)
        return n > 253 ? 0 : 0;
    /* Label start offsets, from the right. */
    size_t starts[128];
    size_t nl = 0;
    size_t i = n;
    for (;;) {
        size_t j = i;
        while (j > 0 && host[j - 1] != '.')
            j--;
        if (nl == 128)
            return 0;
        starts[nl++] = j;
        if (j == 0)
            break;
        i = j - 1;
    }
    /* starts[k-1] is where the suffix of k labels begins. */
    size_t best = 1;
    char key[256];
    size_t kn = 0;
    for (size_t k = 1; k <= nl; k++) {
        /* Append label k (from the right) to the reversed key. */
        size_t ls = starts[k - 1];
        size_t le = k == 1 ? n : starts[k - 2] - 1;
        if (kn + (k > 1) + (le - ls) >= sizeof key)
            break;
        if (k > 1)
            key[kn++] = '.';
        memcpy(key + kn, host + ls, le - ls);
        kn += le - ls;
        int f = lookup(key, kn);
        if (f & 4)
            return n - (k >= 2 ? starts[k - 2] : n); /* exception: one label fewer */
        if ((f & 1) && k > best)
            best = k;
        if ((f & 2) && nl > k && k + 1 > best)
            best = k + 1;
    }
    if (best > nl)
        best = nl;
    return n - starts[best - 1];
}

static bool is_ip(const char *h)
{
    if (h[0] == '[')
        return true;
    /* The URL parser turns numeric hosts into dotted IPv4; a host ending
     * in a digit label is an address. */
    size_t n = strlen(h);
    return n && au_is_digit((unsigned char)h[n - 1]);
}

bool au_psl_is_public_suffix(const char *host)
{
    size_t n = strlen(host);
    return n && !is_ip(host) && au_psl_suffix_len(host, n) == n;
}

bool au_psl_registrable(const char *host, const char **dom, size_t *len)
{
    size_t n = strlen(host);
    if (!n || is_ip(host))
        return false;
    size_t sl = au_psl_suffix_len(host, n);
    if (sl >= n)
        return false;
    /* One more label to the left of the suffix (which is preceded by '.'). */
    size_t start = n - sl - 1;
    while (start > 0 && host[start - 1] != '.')
        start--;
    *dom = host + start;
    *len = n - start;
    return true;
}

void au_url_site(const AuUrl *u, AuBuf *out)
{
    au_buf_clear(out);
    if (u->host_type == AU_URL_HOST_NULL || !u->special || !strcmp(u->scheme, "file")) {
        au_buf_puts(out, "null");
        return;
    }
    au_buf_puts(out, u->scheme);
    au_buf_puts(out, "://");
    const char *d;
    size_t dl;
    if (au_psl_registrable(u->host, &d, &dl))
        au_buf_append(out, d, dl);
    else
        au_buf_puts(out, u->host);
    au_buf_cstr(out);
}

bool au_same_site(const AuUrl *a, const AuUrl *b)
{
    AuBuf x, y;
    au_buf_init(&x);
    au_buf_init(&y);
    au_url_site(a, &x);
    au_url_site(b, &y);
    bool same = strcmp(au_buf_cstr(&x), "null") && !strcmp(au_buf_cstr(&x), au_buf_cstr(&y));
    au_buf_free(&x);
    au_buf_free(&y);
    return same;
}

bool au_same_registrable(const char *a, const char *b)
{
    const char *da, *db;
    size_t la, lb;
    bool ra = au_psl_registrable(a, &da, &la), rb = au_psl_registrable(b, &db, &lb);
    if (!ra)
        da = a, la = strlen(a);
    if (!rb)
        db = b, lb = strlen(b);
    return la == lb && !memcmp(da, db, la);
}
