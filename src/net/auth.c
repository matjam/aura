/* HTTP Basic and Digest authentication, and Base64. */
#include "auth.h"
#include "headers.h"

#include "bearssl.h"

#include <stdio.h>
#include <string.h>

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void au_base64_encode(const void *data, size_t len, AuBuf *out)
{
    const uint8_t *p = data;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        char o[4] = {b64[v >> 18], b64[v >> 12 & 63], b64[v >> 6 & 63], b64[v & 63]};
        au_buf_append(out, o, 4);
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        char o[4] = {b64[v >> 18], b64[v >> 12 & 63], '=', '='};
        au_buf_append(out, o, 4);
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        char o[4] = {b64[v >> 18], b64[v >> 12 & 63], b64[v >> 6 & 63], '='};
        au_buf_append(out, o, 4);
    }
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool au_base64_decode(const char *s, size_t n, AuBuf *out)
{
    /* Forgiving-base64: drop ASCII whitespace, then at most two '=' at
     * the end when the length is a multiple of 4; length mod 4 must not
     * be 1. */
    size_t len = 0, pad = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (au_is_space(c))
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad || b64val(c) < 0)
            return false; /* '=' in the middle, or a bad character */
        len++;
    }
    if (pad > 2 || (pad && (len + pad) % 4 != 0) || len % 4 == 1)
        return false;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64val((unsigned char)s[i]);
        if (v < 0)
            continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            au_buf_putc(out, (char)(acc >> bits & 0xFF));
        }
    }
    return true;
}

/* ---- Challenges ---- */

/* Copies a parameter value (token or quoted) from *s into out (cap). */
static void get_value(const char **sp, char *out, size_t cap)
{
    const char *s = *sp;
    size_t n = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1])
                s++;
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
        if (*s == '"')
            s++;
    } else {
        while (*s && *s != ',' && *s != ' ' && *s != '\t') {
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
    }
    out[n] = 0;
    *sp = s;
}

static int alg_rank(const char *a)
{
    if (!a[0] || !strcmp(a, "MD5") || !strcmp(a, "md5"))
        return 1;
    if (!strcmp(a, "SHA-256") || !strcmp(a, "sha-256"))
        return 2;
    return 0;
}

bool au_auth_parse(const char *value, AuAuthParams *best)
{
    int best_rank = -1;
    const char *s = value;
    while (s && *s) {
        while (*s == ' ' || *s == '\t' || *s == ',')
            s++;
        const char *sch = s;
        while (*s && au_http_is_tchar((unsigned char)*s))
            s++;
        size_t sl = (size_t)(s - sch);
        if (!sl)
            break;
        AuAuthParams p;
        memset(&p, 0, sizeof p);
        bool basic = au_ieq(sch, sl, "Basic", 5), digest = au_ieq(sch, sl, "Digest", 6);
        bool qop_seen = false;
        /* Parameters: name=value pairs until the next scheme token (a
         * token not followed by '='). */
        for (;;) {
            while (*s == ' ' || *s == '\t' || *s == ',')
                s++;
            const char *pn = s;
            while (*s && au_http_is_tchar((unsigned char)*s))
                s++;
            size_t pl = (size_t)(s - pn);
            const char *q = s;
            while (*q == ' ' || *q == '\t')
                q++;
            if (!pl || *q != '=') {
                s = pn; /* a new scheme */
                break;
            }
            s = q + 1;
            while (*s == ' ' || *s == '\t')
                s++;
            char val[256];
            get_value(&s, val, sizeof val);
            if (au_ieq(pn, pl, "realm", 5))
                snprintf(p.realm, sizeof p.realm, "%s", val);
            else if (au_ieq(pn, pl, "nonce", 5))
                snprintf(p.nonce, sizeof p.nonce, "%s", val);
            else if (au_ieq(pn, pl, "opaque", 6))
                snprintf(p.opaque, sizeof p.opaque, "%s", val);
            else if (au_ieq(pn, pl, "algorithm", 9))
                snprintf(p.algorithm, sizeof p.algorithm, "%s", val);
            else if (au_ieq(pn, pl, "stale", 5))
                p.stale = au_ieq(val, strlen(val), "true", 4);
            else if (au_ieq(pn, pl, "userhash", 8))
                p.userhash = au_ieq(val, strlen(val), "true", 4);
            else if (au_ieq(pn, pl, "qop", 3)) {
                qop_seen = true;
                const char *v = val, *item;
                size_t il;
                while (au_http_list_next(&v, &item, &il))
                    if (au_ieq(item, il, "auth", 4))
                        p.qop_auth = true;
            }
        }
        int rank = -1;
        if (basic) {
            snprintf(p.scheme, sizeof p.scheme, "Basic");
            rank = 0;
        } else if (digest && p.nonce[0] && alg_rank(p.algorithm) > 0 && (!qop_seen || p.qop_auth)) {
            snprintf(p.scheme, sizeof p.scheme, "Digest");
            rank = 1 + alg_rank(p.algorithm);
        }
        if (rank > best_rank) {
            best_rank = rank;
            *best = p;
        }
    }
    return best_rank >= 0;
}

void au_auth_basic(const char *user, const char *pass, AuBuf *out)
{
    AuBuf cred;
    au_buf_init(&cred);
    au_buf_puts(&cred, user);
    au_buf_putc(&cred, ':');
    au_buf_puts(&cred, pass);
    au_buf_puts(out, "Basic ");
    au_base64_encode(cred.data, cred.len, out);
    au_buf_free(&cred);
}

static void hexdigest(bool sha256, const char *s, size_t n, char *out)
{
    uint8_t d[32];
    size_t dl;
    if (sha256) {
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, s, n);
        br_sha256_out(&c, d);
        dl = 32;
    } else {
        br_md5_context c;
        br_md5_init(&c);
        br_md5_update(&c, s, n);
        br_md5_out(&c, d);
        dl = 16;
    }
    for (size_t i = 0; i < dl; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
    out[2 * dl] = 0;
}

static void quoted(AuBuf *out, const char *name, const char *v)
{
    au_buf_printf(out, "%s=\"", name);
    for (; *v; v++) {
        if (*v == '"' || *v == '\\')
            au_buf_putc(out, '\\');
        au_buf_putc(out, *v);
    }
    au_buf_putc(out, '"');
}

void au_auth_digest(const AuAuthParams *p, const char *user, const char *pass, const char *method, const char *uri,
                    uint32_t nc, const char *cnonce, AuBuf *out)
{
    bool sha = p->algorithm[0] && (p->algorithm[0] == 'S' || p->algorithm[0] == 's');
    bool sess = strstr(p->algorithm, "-sess") || strstr(p->algorithm, "-SESS");
    char ha1[65], ha2[65], resp[65], unh[65];
    AuBuf t;
    au_buf_init(&t);
    au_buf_printf(&t, "%s:%s:%s", user, p->realm, pass);
    hexdigest(sha, t.data, t.len, ha1);
    if (sess) {
        au_buf_clear(&t);
        au_buf_printf(&t, "%s:%s:%s", ha1, p->nonce, cnonce);
        hexdigest(sha, t.data, t.len, ha1);
    }
    au_buf_clear(&t);
    au_buf_printf(&t, "%s:%s", method, uri);
    hexdigest(sha, t.data, t.len, ha2);
    au_buf_clear(&t);
    if (p->qop_auth)
        au_buf_printf(&t, "%s:%s:%08x:%s:auth:%s", ha1, p->nonce, (unsigned)nc, cnonce, ha2);
    else
        au_buf_printf(&t, "%s:%s:%s", ha1, p->nonce, ha2);
    hexdigest(sha, t.data, t.len, resp);
    au_buf_puts(out, "Digest ");
    if (p->userhash) {
        au_buf_clear(&t);
        au_buf_printf(&t, "%s:%s", user, p->realm);
        hexdigest(sha, t.data, t.len, unh);
        quoted(out, "username", unh);
        au_buf_puts(out, ", userhash=true");
    } else {
        quoted(out, "username", user);
    }
    au_buf_puts(out, ", ");
    quoted(out, "realm", p->realm);
    au_buf_puts(out, ", ");
    quoted(out, "nonce", p->nonce);
    au_buf_puts(out, ", ");
    quoted(out, "uri", uri);
    if (p->algorithm[0])
        au_buf_printf(out, ", algorithm=%s", p->algorithm);
    au_buf_puts(out, ", ");
    quoted(out, "response", resp);
    if (p->opaque[0]) {
        au_buf_puts(out, ", ");
        quoted(out, "opaque", p->opaque);
    }
    if (p->qop_auth) {
        au_buf_printf(out, ", qop=auth, nc=%08x, ", (unsigned)nc);
        quoted(out, "cnonce", cnonce);
    }
    au_buf_free(&t);
}
