/* data: URLs (see dataurl.h). */
#include "dataurl.h"
#include "auth.h"
#include "headers.h"
#include "../url/url.h"

#include <string.h>

void au_data_url_init(AuDataUrl *d)
{
    au_buf_init(&d->mime);
    au_buf_init(&d->charset);
    au_buf_init(&d->body);
}

void au_data_url_free(AuDataUrl *d)
{
    au_buf_free(&d->mime);
    au_buf_free(&d->charset);
    au_buf_free(&d->body);
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool au_data_url_decode(const char *input, AuDataUrl *d)
{
    au_buf_clear(&d->mime);
    au_buf_clear(&d->charset);
    au_buf_clear(&d->body);
    AuUrl u;
    if (!au_url_parse(&u, input, NULL))
        return false;
    bool ok = false;
    AuBuf s;
    au_buf_init(&s);
    if (strcmp(u.scheme, "data") != 0)
        goto out;
    au_url_serialize_no_fragment(&u, &s);
    const char *p = au_buf_cstr(&s) + 5; /* after "data:" */
    const char *comma = strchr(p, ',');
    if (!comma)
        goto out;
    /* The type: leading and trailing ASCII whitespace removed. */
    const char *ts = p, *te = comma;
    while (ts < te && is_ws(*ts))
        ts++;
    while (te > ts && is_ws(te[-1]))
        te--;
    /* Body: percent-decoded. */
    AuBuf body;
    au_buf_init(&body);
    au_pct_decode(&body, comma + 1, strlen(comma + 1));
    /* ";base64" at the end of the type (spaces allowed before it). */
    bool b64 = false;
    if (te - ts >= 6) {
        const char *q = te - 6;
        if (!au_ieq(q, 6, "base64", 6)) {
            /* not base64 */
        } else {
            const char *r = q;
            while (r > ts && (r[-1] == ' '))
                r--;
            if (r > ts && r[-1] == ';') {
                b64 = true;
                te = r - 1;
                while (te > ts && (te[-1] == ' '))
                    te--;
            }
        }
    }
    if (b64) {
        AuBuf dec;
        au_buf_init(&dec);
        if (!au_base64_decode(body.data ? body.data : "", body.len, &dec)) {
            au_buf_free(&dec);
            au_buf_free(&body);
            goto out;
        }
        au_buf_free(&body);
        body = dec;
    }
    d->body = body;
    /* The type: ";charset=x" alone means text/plain;charset=x. */
    AuBuf type;
    au_buf_init(&type);
    if (ts < te && *ts == ';')
        au_buf_puts(&type, "text/plain");
    au_buf_append(&type, ts, (size_t)(te - ts));
    if (!au_http_parse_mime(au_buf_cstr(&type), &d->mime, &d->charset)) {
        au_buf_clear(&d->mime);
        au_buf_clear(&d->charset);
        au_buf_puts(&d->mime, "text/plain");
        au_buf_puts(&d->charset, "US-ASCII");
    }
    au_buf_free(&type);
    au_buf_cstr(&d->mime);
    au_buf_cstr(&d->charset);
    ok = true;
out:
    au_buf_free(&s);
    au_url_free(&u);
    return ok;
}
