/* Referrer Policy (W3C Referrer Policy, "determine request's referrer")
 * and the Fetch Metadata request headers (Sec-Fetch-*). */
#include "policy.h"

#include <string.h>

static const struct {
    const char *name;
    AuReferrerPolicy p;
} policies[] = {
    {"no-referrer", AU_RP_NO_REFERRER},
    {"no-referrer-when-downgrade", AU_RP_NO_REFERRER_WHEN_DOWNGRADE},
    {"same-origin", AU_RP_SAME_ORIGIN},
    {"origin", AU_RP_ORIGIN},
    {"strict-origin", AU_RP_STRICT_ORIGIN},
    {"origin-when-cross-origin", AU_RP_ORIGIN_WHEN_CROSS_ORIGIN},
    {"strict-origin-when-cross-origin", AU_RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN},
    {"unsafe-url", AU_RP_UNSAFE_URL},
};

AuReferrerPolicy au_referrer_policy_token(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof policies / sizeof policies[0]; i++)
        if (au_ieq(s, n, policies[i].name, strlen(policies[i].name)))
            return policies[i].p;
    return AU_RP_DEFAULT;
}

AuReferrerPolicy au_referrer_policy_parse(const char *value, AuReferrerPolicy current)
{
    /* The last recognised token wins (so fallbacks can be listed first). */
    const char *item;
    size_t il;
    AuReferrerPolicy p = current;
    while (value && au_http_list_next(&value, &item, &il)) {
        AuReferrerPolicy t = au_referrer_policy_token(item, il);
        if (t != AU_RP_DEFAULT)
            p = t;
    }
    return p;
}

static bool potentially_trustworthy(const AuUrl *u)
{
    if (!strcmp(u->scheme, "https") || !strcmp(u->scheme, "wss") || !strcmp(u->scheme, "file"))
        return true;
    if (u->host_type == AU_URL_HOST_IPV4 && (u->ipv4 >> 24) == 127)
        return true;
    if (u->host_type == AU_URL_HOST_IPV6 && !strcmp(u->host, "[::1]"))
        return true;
    size_t n = strlen(u->host);
    return u->host_type == AU_URL_HOST_DOMAIN &&
           (!strcmp(u->host, "localhost") || (n > 10 && !strcmp(u->host + n - 10, ".localhost")));
}

bool au_url_trustworthy(const AuUrl *u)
{
    return potentially_trustworthy(u);
}

/* "strip url for use as a referrer": no credentials or fragment; with
 * origin_only, no path or query either. */
static void strip(const AuUrl *u, bool origin_only, AuBuf *out)
{
    au_buf_clear(out);
    au_buf_puts(out, u->scheme);
    au_buf_puts(out, "://");
    au_buf_puts(out, u->host);
    if (u->port >= 0)
        au_buf_printf(out, ":%d", u->port);
    if (origin_only) {
        au_buf_putc(out, '/');
    } else {
        au_buf_puts(out, u->path);
        if (u->query) {
            au_buf_putc(out, '?');
            au_buf_puts(out, u->query);
        }
    }
    au_buf_cstr(out);
}

bool au_referrer_compute(const char *referrer, const AuUrl *target, AuReferrerPolicy policy, AuBuf *out)
{
    au_buf_clear(out);
    if (!referrer)
        return false;
    AuUrl r;
    if (!au_url_parse(&r, referrer, NULL))
        return false;
    bool ok = false;
    /* Only http(s) referrers are sent (not file:, data:, about:). */
    if (strcmp(r.scheme, "http") && strcmp(r.scheme, "https"))
        goto done;
    if (policy == AU_RP_DEFAULT)
        policy = AU_RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN;
    AuOrigin ro, to;
    au_url_origin(&r, &ro);
    au_url_origin(target, &to);
    bool same = au_origin_same(&ro, &to);
    bool downgrade = potentially_trustworthy(&r) && !potentially_trustworthy(target);
    au_origin_free(&ro);
    au_origin_free(&to);
    switch (policy) {
    case AU_RP_NO_REFERRER:
        break;
    case AU_RP_ORIGIN:
        strip(&r, true, out);
        ok = true;
        break;
    case AU_RP_UNSAFE_URL:
        strip(&r, false, out);
        ok = true;
        break;
    case AU_RP_STRICT_ORIGIN:
        if (!downgrade) {
            strip(&r, true, out);
            ok = true;
        }
        break;
    case AU_RP_SAME_ORIGIN:
        if (same) {
            strip(&r, false, out);
            ok = true;
        }
        break;
    case AU_RP_ORIGIN_WHEN_CROSS_ORIGIN:
        strip(&r, !same, out);
        ok = true;
        break;
    case AU_RP_NO_REFERRER_WHEN_DOWNGRADE:
        if (!downgrade) {
            strip(&r, false, out);
            ok = true;
        }
        break;
    case AU_RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN:
    default:
        if (same) {
            strip(&r, false, out);
            ok = true;
        } else if (!downgrade) {
            strip(&r, true, out);
            ok = true;
        }
        break;
    }
    /* A referrer longer than 4096 bytes is cut to its origin. */
    if (ok && out->len > 4096)
        strip(&r, true, out);
done:
    au_url_free(&r);
    return ok;
}

const char *au_sec_fetch_site(const char *initiator, const AuUrl *target)
{
    if (!initiator)
        return "none";
    AuUrl r;
    if (!au_url_parse(&r, initiator, NULL))
        return "cross-site";
    AuOrigin ro, to;
    au_url_origin(&r, &ro);
    au_url_origin(target, &to);
    const char *res;
    if (au_origin_same(&ro, &to))
        res = "same-origin";
    else if (au_same_site(&r, target))
        res = "same-site";
    else
        res = "cross-site";
    au_origin_free(&ro);
    au_origin_free(&to);
    au_url_free(&r);
    return res;
}

const char *au_sec_fetch_dest(AuRequestDest d)
{
    switch (d) {
    case AU_DEST_DOCUMENT: return "document";
    case AU_DEST_IFRAME: return "iframe";
    case AU_DEST_STYLE: return "style";
    case AU_DEST_SCRIPT: return "script";
    case AU_DEST_IMAGE: return "image";
    case AU_DEST_FONT: return "font";
    default: return "empty";
    }
}

const char *au_sec_fetch_mode(AuRequestMode m)
{
    switch (m) {
    case AU_MODE_NAVIGATE: return "navigate";
    case AU_MODE_CORS: return "cors";
    case AU_MODE_SAME_ORIGIN: return "same-origin";
    default: return "no-cors";
    }
}

const char *au_accept_for(AuRequestDest d)
{
    switch (d) {
    case AU_DEST_DOCUMENT:
    case AU_DEST_IFRAME:
        return "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
    case AU_DEST_STYLE: return "text/css,*/*;q=0.1";
    case AU_DEST_IMAGE: return "image/png,image/jpeg,image/gif,image/webp,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
    case AU_DEST_SCRIPT: return "*/*";
    case AU_DEST_FONT: return "application/font-woff2;q=1.0,application/font-woff;q=0.9,*/*;q=0.8";
    default: return "*/*";
    }
}
