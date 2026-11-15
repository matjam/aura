/* URL hosts: IPv4 and IPv6 parsing and serialization, opaque hosts, the
 * reduced domain-to-ASCII (see url.h), and Punycode (RFC 3492). */
#include "url_int.h"

#include <string.h>

/* ---- Punycode ---- */

enum { PC_BASE = 36, PC_TMIN = 1, PC_TMAX = 26, PC_SKEW = 38, PC_DAMP = 700, PC_IBIAS = 72, PC_IN = 128 };

static uint32_t pc_adapt(uint32_t delta, uint32_t numpoints, bool first)
{
    delta = first ? delta / PC_DAMP : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((PC_BASE - PC_TMIN) * PC_TMAX) / 2) {
        delta /= PC_BASE - PC_TMIN;
        k += PC_BASE;
    }
    return k + (PC_BASE - PC_TMIN + 1) * delta / (delta + PC_SKEW);
}

static char pc_digit(uint32_t d)
{
    return (char)(d < 26 ? 'a' + d : '0' + (d - 26));
}

bool au_punycode_encode(const uint32_t *cp, size_t n, AuBuf *out)
{
    if (n > 4096)
        return false;
    uint32_t b = 0;
    for (size_t j = 0; j < n; j++)
        if (cp[j] < 0x80) {
            au_buf_putc(out, (char)cp[j]);
            b++;
        }
    uint32_t h = b, nn = PC_IN, delta = 0, bias = PC_IBIAS;
    if (b > 0)
        au_buf_putc(out, '-');
    while (h < n) {
        uint32_t m = 0xFFFFFFFFu;
        for (size_t j = 0; j < n; j++)
            if (cp[j] >= nn && cp[j] < m)
                m = cp[j];
        if ((uint64_t)(m - nn) * (h + 1) + delta > 0xFFFFFFFFu)
            return false;
        delta += (m - nn) * (h + 1);
        nn = m;
        for (size_t j = 0; j < n; j++) {
            if (cp[j] < nn) {
                if (++delta == 0)
                    return false;
            }
            if (cp[j] == nn) {
                uint32_t q = delta;
                for (uint32_t k = PC_BASE;; k += PC_BASE) {
                    uint32_t t = k <= bias ? PC_TMIN : k >= bias + PC_TMAX ? PC_TMAX : k - bias;
                    if (q < t)
                        break;
                    au_buf_putc(out, pc_digit(t + (q - t) % (PC_BASE - t)));
                    q = (q - t) / (PC_BASE - t);
                }
                au_buf_putc(out, pc_digit(q));
                bias = pc_adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        nn++;
    }
    return true;
}

int au_punycode_decode(const char *s, size_t n, uint32_t *out, size_t cap)
{
    size_t b = 0, outlen = 0;
    for (size_t j = 0; j < n; j++)
        if (s[j] == '-')
            b = j;
    if (b > cap)
        return -1;
    for (size_t j = 0; j < b; j++) {
        if ((unsigned char)s[j] >= 0x80)
            return -1;
        out[outlen++] = (unsigned char)s[j];
    }
    uint32_t nn = PC_IN, i = 0, bias = PC_IBIAS;
    for (size_t in = b > 0 ? b + 1 : 0; in < n;) {
        uint32_t oldi = i, w = 1;
        for (uint32_t k = PC_BASE;; k += PC_BASE) {
            if (in >= n)
                return -1;
            int c = (unsigned char)s[in++];
            uint32_t digit;
            if (c >= '0' && c <= '9')
                digit = (uint32_t)(c - '0' + 26);
            else if (c >= 'a' && c <= 'z')
                digit = (uint32_t)(c - 'a');
            else if (c >= 'A' && c <= 'Z')
                digit = (uint32_t)(c - 'A');
            else
                return -1;
            if (digit > (0xFFFFFFFFu - i) / w)
                return -1;
            i += digit * w;
            uint32_t t = k <= bias ? PC_TMIN : k >= bias + PC_TMAX ? PC_TMAX : k - bias;
            if (digit < t)
                break;
            if (w > 0xFFFFFFFFu / (PC_BASE - t))
                return -1;
            w *= PC_BASE - t;
        }
        bias = pc_adapt(i - oldi, (uint32_t)outlen + 1, oldi == 0);
        if (i / (outlen + 1) > 0xFFFFFFFFu - nn)
            return -1;
        nn += i / (uint32_t)(outlen + 1);
        i %= (uint32_t)(outlen + 1);
        if (outlen >= cap || nn > 0x10FFFF || (nn >= 0xD800 && nn <= 0xDFFF) || nn < 0x80)
            return -1;
        memmove(out + i + 1, out + i, (outlen - i) * sizeof *out);
        out[i++] = nn;
        outlen++;
    }
    return (int)outlen;
}

/* ---- Code point classes ---- */

bool url_forbidden_host(uint32_t c)
{
    switch (c) {
    case 0: case '\t': case '\n': case '\r': case ' ': case '#': case '/': case ':': case '<':
    case '>': case '?': case '@': case '[': case '\\': case ']': case '^': case '|':
        return true;
    default:
        return false;
    }
}

static bool forbidden_domain(uint32_t c)
{
    return url_forbidden_host(c) || c < 0x20 || c == '%' || c == 0x7F;
}

/* ---- IPv4 ---- */

/* Parses one IPv4 part. Returns false for failure. Values are clamped to
 * 2^32 (anything that large fails later). */
static bool ipv4_number(const char *s, size_t n, uint64_t *v)
{
    if (n == 0)
        return false;
    unsigned radix = 10;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        n -= 2;
        radix = 16;
    } else if (n >= 2 && s[0] == '0') {
        s++;
        n--;
        radix = 8;
    }
    uint64_t r = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A' + 10);
        else
            return false;
        if (d >= radix)
            return false;
        r = r * radix + d;
        if (r > 0x100000000ull)
            r = 0x100000000ull + 1;
    }
    *v = r;
    return true;
}

static bool ends_in_number(const char *s, size_t n)
{
    /* Last label, ignoring one trailing empty label. */
    size_t end = n;
    if (end && s[end - 1] == '.')
        end--;
    size_t start = end;
    while (start > 0 && s[start - 1] != '.')
        start--;
    if (start == end)
        return false;
    bool digits = true;
    for (size_t i = start; i < end; i++)
        if (!au_is_digit((unsigned char)s[i]))
            digits = false;
    if (digits)
        return true;
    uint64_t v;
    return end - start >= 2 && s[start] == '0' && (s[start + 1] == 'x' || s[start + 1] == 'X') &&
           ipv4_number(s + start, end - start, &v);
}

static bool ipv4_parse(const char *s, size_t n, uint32_t *out)
{
    uint64_t nums[4];
    size_t count = 0;
    size_t end = n;
    if (end && s[end - 1] == '.')
        end--; /* a trailing dot is allowed */
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < end && s[j] != '.')
            j++;
        if (count == 4)
            return false;
        if (!ipv4_number(s + i, j - i, &nums[count]))
            return false;
        count++;
        if (j >= end)
            break;
        i = j + 1;
    }
    for (size_t k = 0; k + 1 < count; k++)
        if (nums[k] > 255)
            return false;
    if (nums[count - 1] >= (1ull << (8 * (5 - count))))
        return false;
    uint64_t v = nums[count - 1];
    for (size_t k = 0; k + 1 < count; k++)
        v += nums[k] << (8 * (3 - k));
    *out = (uint32_t)v;
    return true;
}

/* ---- IPv6 ---- */

static int hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool ipv6_parse(const char *s, size_t n, uint16_t addr[8])
{
    memset(addr, 0, 16);
    int piece = 0, compress = -1;
    size_t p = 0;
#define C (p < n ? (unsigned char)s[p] : -1)
    if (C == ':') {
        if (p + 1 >= n || s[p + 1] != ':')
            return false;
        p += 2;
        piece++;
        compress = piece;
    }
    while (C != -1) {
        if (piece == 8)
            return false;
        if (C == ':') {
            if (compress != -1)
                return false;
            p++;
            piece++;
            compress = piece;
            continue;
        }
        unsigned value = 0;
        int length = 0;
        while (length < 4 && C != -1 && hexv(C) >= 0) {
            value = value * 16 + (unsigned)hexv(C);
            p++;
            length++;
        }
        if (C == '.') {
            if (length == 0)
                return false;
            p -= (size_t)length;
            if (piece > 6)
                return false;
            int seen = 0;
            while (C != -1) {
                int v4 = -1;
                if (seen > 0) {
                    if (C == '.' && seen < 4)
                        p++;
                    else
                        return false;
                }
                if (C == -1 || !au_is_digit(C))
                    return false;
                while (C != -1 && au_is_digit(C)) {
                    int d = C - '0';
                    if (v4 == -1)
                        v4 = d;
                    else if (v4 == 0)
                        return false;
                    else
                        v4 = v4 * 10 + d;
                    if (v4 > 255)
                        return false;
                    p++;
                }
                addr[piece] = (uint16_t)(addr[piece] * 0x100 + v4);
                seen++;
                if (seen == 2 || seen == 4)
                    piece++;
            }
            if (seen != 4)
                return false;
            break;
        } else if (C == ':') {
            p++;
            if (C == -1)
                return false;
        } else if (C != -1) {
            return false;
        }
        addr[piece] = (uint16_t)value;
        piece++;
    }
#undef C
    if (compress != -1) {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0) {
            uint16_t t = addr[piece];
            addr[piece] = addr[compress + swaps - 1];
            addr[compress + swaps - 1] = t;
            piece--;
            swaps--;
        }
    } else if (piece != 8) {
        return false;
    }
    return true;
}

void url_ipv6_serialize(const uint16_t a[8], AuBuf *out)
{
    int best = -1, bestlen = 1;
    for (int i = 0; i < 8;) {
        if (a[i] != 0) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && a[j] == 0)
            j++;
        if (j - i > bestlen) {
            best = i;
            bestlen = j - i;
        }
        i = j;
    }
    au_buf_putc(out, '[');
    bool ignore0 = false;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && a[i] == 0)
            continue;
        ignore0 = false;
        if (best == i) {
            au_buf_puts(out, i == 0 ? "::" : ":");
            ignore0 = true;
            continue;
        }
        au_buf_printf(out, "%x", a[i]);
        if (i != 7)
            au_buf_putc(out, ':');
    }
    au_buf_putc(out, ']');
}

/* ---- Domain to ASCII (reduced UTS #46; see url.h) ---- */

/* Returns the mapped code point, IDNA_DROP to drop it, or 0xFFFFFFFF when
 * disallowed. */
#define IDNA_DROP 0xFFFFFFFEu

static uint32_t idna_map(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c < 0x80)
        return c;
    if (c == 0x3002 || c == 0xFF0E || c == 0xFF61)
        return '.';
    /* Fullwidth ASCII maps to ASCII (then lowercased); the ideographic
     * space to a space, which is then forbidden. */
    if (c >= 0xFF01 && c <= 0xFF5E)
        return idna_map(c - 0xFF01 + '!');
    if (c == 0x3000)
        return ' ';
    if ((c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE)
        return 0xFFFFFFFFu; /* noncharacters */
    if (c == 0x00AD || c == 0x034F || c == 0x200B || c == 0x2060 || c == 0xFEFF || (c >= 0x180B && c <= 0x180D) ||
        (c >= 0xFE00 && c <= 0xFE0F))
        return IDNA_DROP;
    if (c == 0xFFFD || (c >= 0x80 && c < 0xA0) || (c >= 0xD800 && c <= 0xDFFF) || c == 0xFFFE || c == 0xFFFF)
        return 0xFFFFFFFFu;
    /* Latin-1 and Greek/Cyrillic capitals: the simple, common cases. */
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    return c;
}

#define MAX_LABEL_CP 256

static bool domain_to_ascii(const char *s, size_t n, AuBuf *out)
{
    /* Decode and map. */
    uint32_t *cps = au_alloc((n + 1) * sizeof *cps);
    size_t ncp = 0;
    for (size_t i = 0; i < n;) {
        size_t adv;
        uint32_t c = au_utf8_decode(s + i, n - i, &adv);
        i += adv;
        uint32_t m = idna_map(c);
        if (m == 0xFFFFFFFFu) {
            au_free(cps);
            return false;
        }
        if (m != IDNA_DROP)
            cps[ncp++] = m;
    }
    bool ok = true;
    size_t start_len = out->len;
    for (size_t i = 0; i <= ncp && ok;) {
        size_t j = i;
        while (j < ncp && cps[j] != '.')
            j++;
        size_t len = j - i;
        bool ascii = true;
        for (size_t k = i; k < j; k++) {
            if (cps[k] >= 0x80)
                ascii = false;
            else if (forbidden_domain(cps[k]))
                ok = false;
        }
        if (!ok)
            break;
        if (ascii) {
            if (len >= 4 && cps[i] == 'x' && cps[i + 1] == 'n' && cps[i + 2] == '-' && cps[i + 3] == '-') {
                char tmp[MAX_LABEL_CP + 1];
                uint32_t dec[MAX_LABEL_CP];
                if (len - 4 > MAX_LABEL_CP) {
                    ok = false;
                    break;
                }
                for (size_t k = 0; k < len - 4; k++)
                    tmp[k] = (char)cps[i + 4 + k];
                int d = au_punycode_decode(tmp, len - 4, dec, MAX_LABEL_CP);
                if (d < 0) {
                    ok = false;
                    break;
                }
                /* The decoded label must contain non-ASCII and map to
                 * itself. A bare "xn--" (empty) is accepted, as browsers
                 * do. */
                bool any = d == 0;
                for (int k = 0; k < d; k++) {
                    if (dec[k] >= 0x80)
                        any = true;
                    if (idna_map(dec[k]) != dec[k] || dec[k] == '.' || forbidden_domain(dec[k]))
                        ok = false;
                }
                if (!any)
                    ok = false;
                if (!ok)
                    break;
            }
            for (size_t k = i; k < j; k++)
                au_buf_putc(out, (char)cps[k]);
        } else {
            if (len > MAX_LABEL_CP) {
                ok = false;
                break;
            }
            au_buf_puts(out, "xn--");
            if (!au_punycode_encode(cps + i, len, out)) {
                ok = false;
                break;
            }
        }
        if (j < ncp)
            au_buf_putc(out, '.');
        i = j + 1;
    }
    au_free(cps);
    if (ok && out->len == start_len)
        ok = false; /* empty domain */
    return ok;
}

bool au_url_host_parse(const char *s, size_t n, bool special, AuBuf *out, AuUrlHostType *type)
{
    uint16_t a6[8];
    uint32_t a4;
    return url_host_parse(s, n, special, out, type, &a4, a6);
}

bool url_host_parse(const char *s, size_t n, bool special, AuBuf *out, AuUrlHostType *type, uint32_t *ipv4,
                    uint16_t ipv6[8])
{
    if (n && s[0] == '[') {
        if (n < 2 || s[n - 1] != ']')
            return false;
        if (!ipv6_parse(s + 1, n - 2, ipv6))
            return false;
        url_ipv6_serialize(ipv6, out);
        *type = AU_URL_HOST_IPV6;
        return true;
    }
    if (!special) {
        for (size_t i = 0; i < n; i++)
            if (url_forbidden_host((unsigned char)s[i]))
                return false;
        au_pct_encode(out, s, n, AU_PCT_C0);
        *type = n ? AU_URL_HOST_OPAQUE : AU_URL_HOST_EMPTY;
        return true;
    }
    if (n == 0)
        return false;
    AuBuf dec, ascii;
    au_buf_init(&dec);
    au_buf_init(&ascii);
    au_pct_decode(&dec, s, n);
    bool ok = domain_to_ascii(dec.data ? dec.data : "", dec.len, &ascii);
    if (ok && ends_in_number(ascii.data, ascii.len)) {
        ok = ipv4_parse(ascii.data, ascii.len, ipv4);
        if (ok) {
            au_buf_printf(out, "%u.%u.%u.%u", (unsigned)(*ipv4 >> 24), (unsigned)(*ipv4 >> 16 & 255),
                          (unsigned)(*ipv4 >> 8 & 255), (unsigned)(*ipv4 & 255));
            *type = AU_URL_HOST_IPV4;
        }
    } else if (ok) {
        au_buf_append(out, ascii.data, ascii.len);
        *type = AU_URL_HOST_DOMAIN;
    }
    au_buf_free(&dec);
    au_buf_free(&ascii);
    return ok;
}
