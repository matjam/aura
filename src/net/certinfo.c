/* A bounded DER reader for certificate details shown to the user:
 * subject, issuer, validity, subjectAltName DNS names, fingerprint.
 * Validation itself is the TLS library's; this only describes. */
#include "tls.h"

#include "auth.h"

#include "core/hash.h"

#include <stdio.h>
#include <string.h>

typedef struct Der {
    const uint8_t *p, *end;
} Der;

/* Reads one TLV. Returns false when malformed or truncated. */
static bool der_next(Der *d, int *tag, Der *content)
{
    if (d->end - d->p < 2)
        return false;
    int t = d->p[0];
    if ((t & 0x1F) == 0x1F)
        return false; /* high tag numbers are not used in certificates */
    size_t len = d->p[1];
    const uint8_t *q = d->p + 2;
    if (len & 0x80) {
        size_t nb = len & 0x7F;
        if (nb == 0 || nb > 4 || (size_t)(d->end - q) < nb)
            return false;
        len = 0;
        for (size_t i = 0; i < nb; i++)
            len = (len << 8) | q[i];
        q += nb;
    }
    if ((size_t)(d->end - q) < len)
        return false;
    *tag = t;
    content->p = q;
    content->end = q + len;
    d->p = q + len;
    return true;
}

static bool der_expect(Der *d, int want, Der *content)
{
    int tag;
    return der_next(d, &tag, content) && tag == want;
}

static void append_text(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    for (size_t i = 0; i < len && *n + 1 < cap; i++)
        out[(*n)++] = s[i];
    out[*n] = 0;
}

static void append_value(char *out, size_t cap, size_t *n, int tag, const Der *v)
{
    size_t len = (size_t)(v->end - v->p);
    if (tag == 0x1E) { /* BMPString: UTF-16BE */
        for (size_t i = 0; i + 1 < len; i += 2) {
            uint32_t cp = (uint32_t)v->p[i] << 8 | v->p[i + 1];
            char u[4];
            int k = au_utf8_encode(cp >= 0xD800 && cp <= 0xDFFF ? 0xFFFD : cp, u);
            append_text(out, cap, n, u, (size_t)k);
        }
        return;
    }
    for (size_t i = 0; i < len;) {
        size_t adv;
        uint32_t cp = au_utf8_decode((const char *)v->p + i, len - i, &adv);
        i += adv;
        if (cp < 0x20 || cp == 0x7F)
            cp = '?';
        char u[4];
        int k = au_utf8_encode(cp, u);
        append_text(out, cap, n, u, (size_t)k);
    }
}

static const char *attr_name(const Der *oid)
{
    static const struct {
        uint8_t b;
        const char *n;
    } names[] = {{3, "CN"}, {6, "C"}, {7, "L"}, {8, "ST"}, {10, "O"}, {11, "OU"}, {5, "serialNumber"}};
    size_t len = (size_t)(oid->end - oid->p);
    if (len == 3 && oid->p[0] == 0x55 && oid->p[1] == 0x04)
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (oid->p[2] == names[i].b)
                return names[i].n;
    static const uint8_t email[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x01};
    if (len == sizeof email && !memcmp(oid->p, email, len))
        return "E";
    return NULL;
}

static bool name_to_text(Der name, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    Der set;
    int tag;
    bool first = true;
    while (name.p < name.end) {
        if (!der_next(&name, &tag, &set) || tag != 0x31)
            return false;
        Der atv;
        while (set.p < set.end) {
            if (!der_expect(&set, 0x30, &atv))
                return false;
            Der oid, val;
            int vt;
            if (!der_expect(&atv, 0x06, &oid) || !der_next(&atv, &vt, &val))
                return false;
            const char *an = attr_name(&oid);
            if (!an)
                continue;
            if (!first)
                append_text(out, cap, &n, ", ", 2);
            first = false;
            append_text(out, cap, &n, an, strlen(an));
            append_text(out, cap, &n, "=", 1);
            append_value(out, cap, &n, vt, &val);
        }
    }
    return true;
}

static int digits(const uint8_t *p, int k)
{
    int v = 0;
    for (int i = 0; i < k; i++) {
        if (!au_is_digit(p[i]))
            return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static bool parse_time(int tag, const Der *v, int64_t *t)
{
    size_t len = (size_t)(v->end - v->p);
    const uint8_t *p = v->p;
    int y;
    if (tag == 0x17 && len == 13 && p[12] == 'Z') {
        y = digits(p, 2);
        if (y < 0)
            return false;
        y += y < 50 ? 2000 : 1900;
        p += 2;
    } else if (tag == 0x18 && len == 15 && p[14] == 'Z') {
        y = digits(p, 4);
        if (y < 0)
            return false;
        p += 4;
    } else {
        return false;
    }
    int mo = digits(p, 2), d = digits(p + 2, 2), h = digits(p + 4, 2), mi = digits(p + 6, 2), s = digits(p + 8, 2);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60)
        return false;
    *t = au_days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

bool au_cert_parse(const uint8_t *der, size_t len, AuCertEntry *e)
{
    memset(e, 0, sizeof *e);
    au_sha256(der, len, e->sha256);
    Der d = {der, der + len}, cert, tbs, x;
    int tag;
    if (!der_expect(&d, 0x30, &cert) || !der_expect(&cert, 0x30, &tbs))
        return false;
    if (!der_next(&tbs, &tag, &x))
        return false;
    if (tag == 0xA0 && !der_next(&tbs, &tag, &x)) /* version, then serial */
        return false;
    if (tag != 0x02)
        return false;
    Der alg, issuer, validity, subject, spki;
    if (!der_expect(&tbs, 0x30, &alg) || !der_expect(&tbs, 0x30, &issuer) || !der_expect(&tbs, 0x30, &validity) ||
        !der_expect(&tbs, 0x30, &subject) || !der_expect(&tbs, 0x30, &spki))
        return false;
    name_to_text(issuer, e->issuer, sizeof e->issuer);
    name_to_text(subject, e->subject, sizeof e->subject);
    Der tv;
    if (der_next(&validity, &tag, &tv))
        parse_time(tag, &tv, &e->not_before);
    if (der_next(&validity, &tag, &tv))
        parse_time(tag, &tv, &e->not_after);
    /* Extensions: [3] { SEQUENCE OF Extension } */
    while (tbs.p < tbs.end) {
        Der opt;
        if (!der_next(&tbs, &tag, &opt))
            return false;
        if (tag != 0xA3)
            continue;
        Der exts, ext;
        if (!der_expect(&opt, 0x30, &exts))
            return false;
        while (exts.p < exts.end) {
            if (!der_expect(&exts, 0x30, &ext))
                return false;
            Der oid, val;
            if (!der_expect(&ext, 0x06, &oid))
                return false;
            if (!der_next(&ext, &tag, &val))
                return false;
            if (tag == 0x01 && !der_next(&ext, &tag, &val)) /* critical flag */
                return false;
            if (tag != 0x04)
                return false;
            static const uint8_t san_oid[] = {0x55, 0x1D, 0x11};
            if (oid.end - oid.p != 3 || memcmp(oid.p, san_oid, 3))
                continue;
            Der names, gn;
            if (!der_expect(&val, 0x30, &names))
                return false;
            size_t n = 0;
            while (names.p < names.end) {
                if (!der_next(&names, &tag, &gn))
                    return false;
                if (tag != 0x82)
                    continue;
                if (n)
                    append_text(e->san, sizeof e->san, &n, ", ", 2);
                append_value(e->san, sizeof e->san, &n, 0x16, &gn);
            }
        }
    }
    return true;
}

/* ---- Path building helpers ----
 * These read hostile bytes (certificates fetched from caIssuers URLs, or
 * sent by a server): every length is checked by der_next, nothing
 * recurses, and nothing is trusted because of what is found here. */

/* Opens a certificate's TBSCertificate and reads up to the subject. The
 * issuer and subject come back as whole TLVs; rest is what follows the
 * subject in the TBS. */
static bool tbs_open(const uint8_t *der, size_t len, AuDerSpan *issuer, AuDerSpan *subject, Der *rest)
{
    Der d = {der, der + len}, cert, tbs, x;
    int tag;
    if (!der_expect(&d, 0x30, &cert) || d.p != d.end || !der_expect(&cert, 0x30, &tbs))
        return false;
    if (!der_next(&tbs, &tag, &x))
        return false;
    if (tag == 0xA0 && !der_next(&tbs, &tag, &x)) /* version, then serial */
        return false;
    if (tag != 0x02)
        return false;
    Der alg, name, validity;
    if (!der_expect(&tbs, 0x30, &alg))
        return false;
    const uint8_t *s = tbs.p;
    if (!der_expect(&tbs, 0x30, &name))
        return false;
    issuer->p = s;
    issuer->len = (size_t)(tbs.p - s);
    if (!der_expect(&tbs, 0x30, &validity))
        return false;
    s = tbs.p;
    if (!der_expect(&tbs, 0x30, &name))
        return false;
    subject->p = s;
    subject->len = (size_t)(tbs.p - s);
    *rest = tbs;
    return true;
}

bool au_x509_names(const uint8_t *der, size_t len, AuDerSpan *issuer, AuDerSpan *subject)
{
    Der rest;
    return der && tbs_open(der, len, issuer, subject, &rest);
}

static bool starts_with_ci(const uint8_t *p, size_t n, const char *pre)
{
    size_t k = strlen(pre);
    if (n < k)
        return false;
    for (size_t i = 0; i < k; i++)
        if (au_lower(p[i]) != (unsigned char)pre[i])
            return false;
    return true;
}

bool au_x509_ca_issuers(const uint8_t *der, size_t len, char *url, size_t cap)
{
    static const uint8_t aia_oid[] = {0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x01};
    static const uint8_t ca_issuers_oid[] = {0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x02};
    if (cap)
        url[0] = 0;
    AuDerSpan iss, subj;
    Der tbs;
    if (!der || !cap || !tbs_open(der, len, &iss, &subj, &tbs))
        return false;
    Der spki;
    if (!der_expect(&tbs, 0x30, &spki))
        return false;
    int tag;
    while (tbs.p < tbs.end) {
        Der opt;
        if (!der_next(&tbs, &tag, &opt))
            return false;
        if (tag != 0xA3)
            continue; /* issuerUniqueID, subjectUniqueID */
        Der exts, ext;
        if (!der_expect(&opt, 0x30, &exts))
            return false;
        while (exts.p < exts.end) {
            Der oid, val;
            if (!der_expect(&exts, 0x30, &ext) || !der_expect(&ext, 0x06, &oid) || !der_next(&ext, &tag, &val))
                return false;
            if (tag == 0x01 && !der_next(&ext, &tag, &val)) /* critical flag */
                return false;
            if (tag != 0x04)
                return false;
            if ((size_t)(oid.end - oid.p) != sizeof aia_oid || memcmp(oid.p, aia_oid, sizeof aia_oid))
                continue;
            /* AuthorityInfoAccessSyntax ::= SEQUENCE OF AccessDescription
             * { accessMethod OID, accessLocation GeneralName } */
            Der seq, ad;
            if (!der_expect(&val, 0x30, &seq))
                return false;
            while (seq.p < seq.end) {
                Der method, loc;
                if (!der_expect(&seq, 0x30, &ad) || !der_expect(&ad, 0x06, &method) || !der_next(&ad, &tag, &loc))
                    return false;
                if ((size_t)(method.end - method.p) != sizeof ca_issuers_oid ||
                    memcmp(method.p, ca_issuers_oid, sizeof ca_issuers_oid) || tag != 0x86)
                    continue; /* OCSP, or not a URI */
                size_t n = (size_t)(loc.end - loc.p);
                if (n + 1 > cap || !starts_with_ci(loc.p, n, "http://"))
                    continue;
                bool printable = true;
                for (size_t i = 0; i < n; i++)
                    if (loc.p[i] <= 0x20 || loc.p[i] >= 0x7F)
                        printable = false;
                if (!printable)
                    continue;
                memcpy(url, loc.p, n);
                url[n] = 0;
                return true;
            }
        }
    }
    return false;
}

/* A whole certificate at the start of d: true and its TLV when it parses
 * as one (the outer SEQUENCE holds a TBS SEQUENCE). */
static bool take_cert(Der *d, AuDerSpan *out)
{
    const uint8_t *s = d->p;
    int tag;
    Der c;
    if (!der_next(d, &tag, &c) || tag != 0x30)
        return false;
    out->p = s;
    out->len = (size_t)(d->p - s);
    AuDerSpan a, b;
    return au_x509_names(out->p, out->len, &a, &b);
}

/* PKCS#7 ContentInfo { signedData, [0] SignedData { version, digest
 * algorithms, contentInfo, [0] IMPLICIT certificates, ... } }. */
static int pkcs7_certs(const uint8_t *data, size_t len, int max,
                       void (*sink)(void *ctx, const uint8_t *der, size_t len), void *ctx)
{
    static const uint8_t signed_data_oid[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02};
    Der d = {data, data + len}, ci, oid, wrap, sd, x, certs;
    int tag;
    if (!der_expect(&d, 0x30, &ci) || !der_expect(&ci, 0x06, &oid) ||
        (size_t)(oid.end - oid.p) != sizeof signed_data_oid || memcmp(oid.p, signed_data_oid, sizeof signed_data_oid))
        return 0;
    if (!der_expect(&ci, 0xA0, &wrap) || !der_expect(&wrap, 0x30, &sd))
        return 0;
    if (!der_expect(&sd, 0x02, &x) || !der_expect(&sd, 0x31, &x) || !der_expect(&sd, 0x30, &x))
        return 0;
    if (!der_next(&sd, &tag, &certs) || tag != 0xA0)
        return 0;
    int n = 0;
    while (certs.p < certs.end && n < max) {
        AuDerSpan c;
        if (!take_cert(&certs, &c))
            break;
        sink(ctx, c.p, c.len);
        n++;
    }
    return n;
}

static const uint8_t *find_text(const uint8_t *p, const uint8_t *end, const char *s)
{
    size_t k = strlen(s);
    for (; (size_t)(end - p) >= k; p++)
        if (!memcmp(p, s, k))
            return p;
    return NULL;
}

int au_cert_bundle_parse(const uint8_t *data, size_t len, int max,
                         void (*sink)(void *ctx, const uint8_t *der, size_t len), void *ctx)
{
    if (!data || !len || max <= 0)
        return 0;
    if (data[0] == 0x30) {
        Der d = {data, data + len};
        AuDerSpan c;
        if (take_cert(&d, &c) && d.p == d.end) {
            sink(ctx, c.p, c.len);
            return 1;
        }
        return pkcs7_certs(data, len, max, sink, ctx);
    }
    /* PEM: CERTIFICATE blocks, or a PKCS7 block. */
    int n = 0;
    const uint8_t *p = data, *end = data + len;
    for (int blocks = 0; blocks < 16 && n < max; blocks++) {
        const uint8_t *b = find_text(p, end, "-----BEGIN ");
        if (!b)
            break;
        const uint8_t *hdr_end = find_text(b + 11, end, "-----");
        if (!hdr_end)
            break;
        bool p7 = starts_with_ci(b + 11, (size_t)(end - b - 11), "pkcs7-----");
        bool cert = starts_with_ci(b + 11, (size_t)(end - b - 11), "certificate-----");
        const uint8_t *body = hdr_end + 5;
        const uint8_t *e = find_text(body, end, "-----END ");
        if (!e)
            break;
        p = e + 9;
        if (!p7 && !cert)
            continue;
        AuBuf der;
        au_buf_init(&der);
        if (au_base64_decode((const char *)body, (size_t)(e - body), &der) && der.len) {
            const uint8_t *q = (const uint8_t *)der.data;
            if (p7) {
                n += pkcs7_certs(q, der.len, max - n, sink, ctx);
            } else {
                Der d = {q, q + der.len};
                AuDerSpan c;
                if (take_cert(&d, &c) && d.p == d.end) {
                    sink(ctx, c.p, c.len);
                    n++;
                }
            }
        }
        au_buf_free(&der);
    }
    return n;
}
