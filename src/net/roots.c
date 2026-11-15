/* Root store loading. See roots.h for the file format. */
#include "roots.h"

#include "tls.h"

#include <stdio.h>
#include <string.h>

typedef struct Root {
    uint8_t *der;
    size_t len;
    size_t subj_off, subj_len; /* the subject Name (TLV) within der */
} Root;

struct AuRootStore {
    Root *roots;
    size_t n, cap;
    int64_t date;
};

AuRootStore *au_roots_new(int64_t date)
{
    AuRootStore *r = au_calloc(sizeof *r);
    r->date = date;
    return r;
}

void au_roots_free(AuRootStore *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->n; i++)
        au_free(r->roots[i].der);
    au_free(r->roots);
    au_free(r);
}

int64_t au_roots_date(const AuRootStore *r) { return r->date; }
size_t au_roots_count(const AuRootStore *r) { return r->n; }

bool au_roots_add_der(AuRootStore *r, const void *der, size_t len)
{
    AuDerSpan iss, subj;
    if (!der || len == 0 || len > AU_ROOT_CERT_MAX || r->n >= AU_ROOTS_MAX || !au_x509_names(der, len, &iss, &subj))
        return false;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->roots = au_realloc(r->roots, r->cap * sizeof *r->roots);
    }
    Root *a = &r->roots[r->n++];
    a->der = au_alloc(len);
    memcpy(a->der, der, len);
    a->len = len;
    a->subj_off = (size_t)(subj.p - (const uint8_t *)der);
    a->subj_len = subj.len;
    return true;
}

bool au_roots_next_by_subject(const AuRootStore *r, const uint8_t *dn, size_t dn_len, size_t *pos,
                              const uint8_t **der, size_t *der_len)
{
    for (size_t i = *pos; i < r->n; i++) {
        const Root *a = &r->roots[i];
        if (a->subj_len == dn_len && !memcmp(a->der + a->subj_off, dn, dn_len)) {
            *der = a->der;
            *der_len = a->len;
            *pos = i + 1;
            return true;
        }
    }
    *pos = r->n;
    return false;
}

bool au_roots_has_subject(const AuRootStore *r, const uint8_t *dn, size_t dn_len)
{
    size_t pos = 0, len;
    const uint8_t *der;
    return au_roots_next_by_subject(r, dn, dn_len, &pos, &der, &len);
}

AuRootStore *au_roots_load(const void *data, size_t len, char *err, size_t errcap)
{
#define BAD(msg)                                                                                                  \
    do {                                                                                                          \
        if (err)                                                                                                  \
            snprintf(err, errcap, "%s", msg);                                                                     \
        au_roots_free(store);                                                                                     \
        return NULL;                                                                                              \
    } while (0)
    AuRootStore *store = NULL;
    const uint8_t *p = data;
    if (!p || len < 16 || memcmp(p, "AURR", 4))
        BAD("not a root store file");
    if (p[4] == 1)
        BAD("a root store in the old format: replace it with the one that came with this version of Aura");
    if (p[4] != 2)
        BAD("unsupported root store version");
    uint32_t count = ((uint32_t)p[6] << 8) | p[7];
    uint64_t date = 0;
    for (int i = 0; i < 8; i++)
        date = (date << 8) | p[8 + i];
    if (count == 0 || count > AU_ROOTS_MAX)
        BAD("bad root store header");
    store = au_roots_new((int64_t)date);
    size_t off = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (len - off < 2)
            BAD("truncated root store");
        size_t n = ((size_t)p[off] << 8) | p[off + 1];
        off += 2;
        if (n == 0 || n > AU_ROOT_CERT_MAX || len - off < n)
            BAD("bad certificate length");
        if (!au_roots_add_der(store, p + off, n))
            BAD("a certificate that cannot be read");
        off += n;
    }
    if (off != len)
        BAD("trailing bytes after the last certificate");
    return store;
#undef BAD
}
