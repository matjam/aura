/* A bounded DER reader for certificate details shown to the user:
 * subject, issuer, validity, subjectAltName DNS names, fingerprint.
 * Validation itself is BearSSL's; this only describes. */
#include "tls.h"

#include "bearssl.h"

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
    br_sha256_context sc;
    br_sha256_init(&sc);
    br_sha256_update(&sc, der, len);
    br_sha256_out(&sc, e->sha256);
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
