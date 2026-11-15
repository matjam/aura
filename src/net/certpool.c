/* The intermediates pool: certificates fetched from caIssuers URLs, kept
 * to complete chains that servers send short. See tls.h. Nothing here is
 * trusted: a pool certificate only takes part in a path that the X.509
 * engine then checks up to a bundled root. */
#include "tls.h"

#include <string.h>

void au_cert_pool_init(AuCertPool *p) { memset(p, 0, sizeof *p); }

void au_cert_pool_free(AuCertPool *p)
{
    for (int i = 0; i < p->n; i++)
        au_free(p->certs[i].der);
    memset(p, 0, sizeof *p);
}

bool au_cert_pool_add(AuCertPool *p, const uint8_t *der, size_t len)
{
    AuDerSpan iss, subj;
    if (!der || len == 0 || len > AU_POOL_CERT_MAX || !au_x509_names(der, len, &iss, &subj))
        return false;
    for (int i = 0; i < p->n; i++)
        if (p->certs[i].len == len && !memcmp(p->certs[i].der, der, len))
            return false;
    if (p->n == AU_POOL_MAX) {
        au_free(p->certs[0].der);
        memmove(&p->certs[0], &p->certs[1], (AU_POOL_MAX - 1) * sizeof p->certs[0]);
        p->n--;
    }
    AuPoolCert *c = &p->certs[p->n++];
    c->der = au_alloc(len);
    memcpy(c->der, der, len);
    c->len = len;
    c->iss_off = (size_t)(iss.p - der);
    c->iss_len = iss.len;
    c->subj_off = (size_t)(subj.p - der);
    c->subj_len = subj.len;
    p->dirty = true;
    return true;
}

void au_cert_pool_serialize(const AuCertPool *p, AuBuf *out)
{
    uint8_t h[8] = {'A', 'U', 'R', 'I', 1, 0, (uint8_t)(p->n >> 8), (uint8_t)p->n};
    au_buf_append(out, h, sizeof h);
    for (int i = 0; i < p->n; i++) {
        uint8_t l[2] = {(uint8_t)(p->certs[i].len >> 8), (uint8_t)p->certs[i].len};
        au_buf_append(out, l, 2);
        au_buf_append(out, p->certs[i].der, p->certs[i].len);
    }
}

bool au_cert_pool_parse(AuCertPool *p, const void *data, size_t len)
{
    const uint8_t *d = data;
    if (!d || len < 8 || memcmp(d, "AURI", 4) || d[4] != 1)
        return false;
    size_t count = (size_t)d[6] << 8 | d[7];
    au_cert_pool_free(p);
    size_t pos = 8;
    for (size_t i = 0; i < count && len - pos >= 2; i++) {
        size_t n = (size_t)d[pos] << 8 | d[pos + 1];
        pos += 2;
        if (n > len - pos)
            break;
        au_cert_pool_add(p, d + pos, n); /* a damaged entry is skipped */
        pos += n;
    }
    p->dirty = false;
    return true;
}
