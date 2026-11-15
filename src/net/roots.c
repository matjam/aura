/* Root store loading. See roots.h for the file format. */
#include "roots.h"

#include "bearssl.h"

#include <stdio.h>
#include <string.h>

struct AuRootStore {
    br_x509_trust_anchor *anchors;
    size_t n, cap;
    int64_t date;
    /* Each anchor's bytes (DN and key) are separate au_alloc blocks so
     * anchors can be added one at a time. */
};

AuRootStore *au_roots_new(int64_t date)
{
    AuRootStore *r = au_calloc(sizeof *r);
    r->date = date;
    return r;
}

static void free_anchor(br_x509_trust_anchor *a)
{
    au_free(a->dn.data);
    if (a->pkey.key_type == BR_KEYTYPE_RSA) {
        au_free(a->pkey.key.rsa.n);
        au_free(a->pkey.key.rsa.e);
    } else {
        au_free(a->pkey.key.ec.q);
    }
}

void au_roots_free(AuRootStore *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->n; i++)
        free_anchor(&r->anchors[i]);
    au_free(r->anchors);
    au_free(r);
}

int64_t au_roots_date(const AuRootStore *r) { return r->date; }
size_t au_roots_count(const AuRootStore *r) { return r->n; }

const void *au_roots_anchors(const AuRootStore *r, size_t *n)
{
    *n = r->n;
    return r->anchors;
}

static br_x509_trust_anchor *push(AuRootStore *r)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->anchors = au_realloc(r->anchors, r->cap * sizeof *r->anchors);
    }
    br_x509_trust_anchor *a = &r->anchors[r->n++];
    memset(a, 0, sizeof *a);
    return a;
}

static uint8_t *copy(const uint8_t *p, size_t n)
{
    uint8_t *q = au_alloc(n ? n : 1);
    memcpy(q, p, n);
    return q;
}

typedef struct Rd {
    const uint8_t *p;
    size_t n;
    bool bad;
} Rd;

static uint32_t rd(Rd *r, int bytes)
{
    if (r->bad || r->n < (size_t)bytes) {
        r->bad = true;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v = (v << 8) | r->p[i];
    r->p += bytes;
    r->n -= (size_t)bytes;
    return v;
}

static const uint8_t *rd_bytes(Rd *r, size_t n)
{
    if (r->bad || r->n < n) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    r->n -= n;
    return p;
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
    Rd r = {data, len, false};
    const uint8_t *magic = rd_bytes(&r, 4);
    if (!magic || memcmp(magic, "AURR", 4))
        BAD("not a root store file");
    if (rd(&r, 1) != 1)
        BAD("unsupported root store version");
    rd(&r, 1);
    uint32_t count = rd(&r, 2);
    uint32_t hi = rd(&r, 4), lo = rd(&r, 4);
    if (r.bad || count == 0 || count > AU_ROOTS_MAX)
        BAD("bad root store header");
    int64_t date = (int64_t)(((uint64_t)hi << 32) | lo);
    store = au_roots_new(date);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t flags = rd(&r, 1);
        uint32_t dnl = rd(&r, 2);
        if (r.bad || dnl == 0 || dnl > 1024)
            BAD("bad anchor name");
        const uint8_t *dn = rd_bytes(&r, dnl);
        uint32_t kt = rd(&r, 1);
        if (r.bad)
            BAD("truncated anchor");
        br_x509_trust_anchor *a = push(store);
        a->dn.data = copy(dn, dnl);
        a->dn.len = dnl;
        a->flags = (flags & 1) ? BR_X509_TA_CA : 0;
        if (kt == 1) {
            uint32_t nl = rd(&r, 2);
            if (r.bad || nl == 0 || nl > 1024)
                BAD("bad RSA modulus");
            const uint8_t *nb = rd_bytes(&r, nl);
            uint32_t el = rd(&r, 2);
            if (r.bad || el == 0 || el > 16)
                BAD("bad RSA exponent");
            const uint8_t *eb = rd_bytes(&r, el);
            if (r.bad)
                BAD("truncated RSA key");
            a->pkey.key_type = BR_KEYTYPE_RSA;
            a->pkey.key.rsa.n = copy(nb, nl);
            a->pkey.key.rsa.nlen = nl;
            a->pkey.key.rsa.e = copy(eb, el);
            a->pkey.key.rsa.elen = el;
        } else if (kt == 2) {
            uint32_t curve = rd(&r, 1);
            uint32_t ql = rd(&r, 2);
            if (r.bad || (curve != BR_EC_secp256r1 && curve != BR_EC_secp384r1 && curve != BR_EC_secp521r1) ||
                ql == 0 || ql > 133)
                BAD("bad EC key");
            const uint8_t *qb = rd_bytes(&r, ql);
            if (r.bad)
                BAD("truncated EC key");
            a->pkey.key_type = BR_KEYTYPE_EC;
            a->pkey.key.ec.curve = (int)curve;
            a->pkey.key.ec.q = copy(qb, ql);
            a->pkey.key.ec.qlen = ql;
        } else {
            /* Undo the half-made anchor before failing. */
            a->pkey.key_type = BR_KEYTYPE_EC;
            a->pkey.key.ec.q = NULL;
            BAD("unknown key type");
        }
    }
    if (r.n != 0)
        BAD("trailing bytes after the last anchor");
    return store;
#undef BAD
}

typedef struct DnBuf {
    AuBuf b;
} DnBuf;

static void dn_append(void *ctx, const void *buf, size_t len)
{
    au_buf_append(&((DnBuf *)ctx)->b, buf, len);
}

bool au_roots_add_der(AuRootStore *r, const void *der, size_t len)
{
    br_x509_decoder_context dc;
    DnBuf dn;
    au_buf_init(&dn.b);
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc) != 0 || !dn.b.len) {
        au_buf_free(&dn.b);
        return false;
    }
    br_x509_trust_anchor *a = push(r);
    a->dn.data = copy((const uint8_t *)dn.b.data, dn.b.len);
    a->dn.len = dn.b.len;
    a->flags = BR_X509_TA_CA;
    a->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        a->pkey.key.rsa.n = copy(pk->key.rsa.n, pk->key.rsa.nlen);
        a->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        a->pkey.key.rsa.e = copy(pk->key.rsa.e, pk->key.rsa.elen);
        a->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else {
        a->pkey.key.ec.curve = pk->key.ec.curve;
        a->pkey.key.ec.q = copy(pk->key.ec.q, pk->key.ec.qlen);
        a->pkey.key.ec.qlen = pk->key.ec.qlen;
    }
    au_buf_free(&dn.b);
    return true;
}
