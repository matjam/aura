/* URL parsing for URL, URLSearchParams, location, anchors, fetch and
 * modules: the WHATWG parser of src/url, with its components laid out as
 * the URL API shows them (protocol with ':', search with '?', hash with
 * '#', the port as a string, "" for each one absent). */
#include "js_int.h"

#include "../url/url.h"

#include <stdio.h>
#include <string.h>

static char *dup_str(const char *prefix, const char *s)
{
    AuBuf b;
    au_buf_init(&b);
    if (prefix)
        au_buf_puts(&b, prefix);
    if (s)
        au_buf_puts(&b, s);
    return au_buf_take(&b, NULL);
}

bool js_url_parse(const char *input, const char *base, JsUrl *out)
{
    memset(out, 0, sizeof *out);
    AuUrl b, u;
    bool have_base = false;
    if (base) {
        if (!au_url_parse(&b, base, NULL))
            return false;
        have_base = true;
    }
    bool ok = au_url_parse(&u, input, have_base ? &b : NULL);
    if (have_base)
        au_url_free(&b);
    if (!ok)
        return false;
    out->href = dup_str(NULL, u.href);
    out->protocol = dup_str(u.scheme, ":");
    out->username = dup_str(NULL, u.username);
    out->password = dup_str(NULL, u.password);
    out->hostname = dup_str(NULL, u.host);
    char port[16] = "";
    if (u.port >= 0)
        snprintf(port, sizeof port, "%d", u.port);
    out->port = dup_str(NULL, port);
    out->pathname = dup_str(NULL, u.path);
    out->search = u.query && u.query[0] ? dup_str("?", u.query) : dup_str(NULL, "");
    out->hash = u.fragment && u.fragment[0] ? dup_str("#", u.fragment) : dup_str(NULL, "");
    au_url_free(&u);
    return true;
}

void js_url_free(JsUrl *u)
{
    au_free(u->href);
    au_free(u->protocol);
    au_free(u->username);
    au_free(u->password);
    au_free(u->hostname);
    au_free(u->port);
    au_free(u->pathname);
    au_free(u->search);
    au_free(u->hash);
    memset(u, 0, sizeof *u);
}
