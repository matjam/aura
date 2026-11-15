/* MIME sniffing (WHATWG MIME Sniffing Standard), mixed content
 * classification and the https upgrade of URLs. */
#include "policy.h"

#include <string.h>

/* ---- Pattern matching (MIME Sniffing 4.1 "pattern matching algorithm") ---- */

typedef struct SniffPattern {
    const char *pattern, *mask;
    size_t len;
    bool skip_ws;
    const char *type;
} SniffPattern;

static bool match(const uint8_t *d, size_t n, const SniffPattern *p)
{
    size_t s = 0;
    if (p->skip_ws)
        while (s < n && (d[s] == 0x09 || d[s] == 0x0A || d[s] == 0x0C || d[s] == 0x0D || d[s] == 0x20))
            s++;
    if (n - s < p->len)
        return false;
    for (size_t i = 0; i < p->len; i++)
        if ((d[s + i] & (uint8_t)p->mask[i]) != (uint8_t)p->pattern[i])
            return false;
    return true;
}

/* HTML signatures need a tag-terminating byte (space or '>') after them. */
static bool match_html(const uint8_t *d, size_t n, const char *sig)
{
    size_t s = 0;
    while (s < n && (d[s] == 0x09 || d[s] == 0x0A || d[s] == 0x0C || d[s] == 0x0D || d[s] == 0x20))
        s++;
    size_t l = strlen(sig);
    if (n - s < l + 1)
        return false;
    for (size_t i = 0; i < l; i++) {
        uint8_t c = d[s + i];
        if (sig[i] >= 'A' && sig[i] <= 'Z')
            c &= 0xDF;
        if (c != (uint8_t)sig[i])
            return false;
    }
    return d[s + l] == 0x20 || d[s + l] == 0x3E;
}

#define P(pat, msk, ws, t) {pat, msk, sizeof(pat) - 1, ws, t}

static const SniffPattern image_patterns[] = {
    P("\x00\x00\x01\x00", "\xFF\xFF\xFF\xFF", false, "image/x-icon"),
    P("\x00\x00\x02\x00", "\xFF\xFF\xFF\xFF", false, "image/x-icon"),
    P("BM", "\xFF\xFF", false, "image/bmp"),
    P("GIF87a", "\xFF\xFF\xFF\xFF\xFF\xFF", false, "image/gif"),
    P("GIF89a", "\xFF\xFF\xFF\xFF\xFF\xFF", false, "image/gif"),
    P("RIFF\x00\x00\x00\x00WEBPVP", "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF", false, "image/webp"),
    P("\x89PNG\r\n\x1A\n", "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", false, "image/png"),
    P("\xFF\xD8\xFF", "\xFF\xFF\xFF", false, "image/jpeg"),
};

static const SniffPattern other_patterns[] = {
    P("%PDF-", "\xFF\xFF\xFF\xFF\xFF", false, "application/pdf"),
    P("%!PS-Adobe-", "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", false, "application/postscript"),
    P("\xFE\xFF\x00\x00", "\xFF\xFF\x00\x00", false, "text/plain"),
    P("\xFF\xFE\x00\x00", "\xFF\xFF\x00\x00", false, "text/plain"),
    P("\xEF\xBB\xBF\x00", "\xFF\xFF\xFF\x00", false, "text/plain"),
    P("\x1F\x8B\x08", "\xFF\xFF\xFF", false, "application/x-gzip"),
    P("PK\x03\x04", "\xFF\xFF\xFF\xFF", false, "application/zip"),
    P("Rar!\x1A\x07\x00", "\xFF\xFF\xFF\xFF\xFF\xFF\xFF", false, "application/x-rar-compressed"),
    P("OggS\x00", "\xFF\xFF\xFF\xFF\xFF", false, "application/ogg"),
    P("ID3", "\xFF\xFF\xFF", false, "audio/mpeg"),
    P("\x00\x00\x00\x00ftyp", "\x00\x00\x00\x00\xFF\xFF\xFF\xFF", false, "video/mp4"),
    P("\x1A\x45\xDF\xA3", "\xFF\xFF\xFF\xFF", false, "video/webm"),
    P("wOFF", "\xFF\xFF\xFF\xFF", false, "font/woff"),
    P("wOF2", "\xFF\xFF\xFF\xFF", false, "font/woff2"),
};

static const char *const html_sigs[] = {"<!DOCTYPE HTML", "<HTML", "<HEAD", "<SCRIPT", "<IFRAME", "<H1", "<DIV",
                                        "<FONT", "<TABLE", "<A", "<STYLE", "<TITLE", "<B", "<BODY", "<BR", "<P",
                                        "<!--"};

static const char *sniff_image(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < sizeof image_patterns / sizeof image_patterns[0]; i++)
        if (match(d, n, &image_patterns[i]))
            return image_patterns[i].type;
    return NULL;
}

static bool binary_byte(uint8_t b)
{
    return b <= 0x08 || b == 0x0B || (b >= 0x0E && b <= 0x1A) || (b >= 0x1C && b <= 0x1F);
}

/* "Rules for identifying an unknown MIME type" with sniff-scriptable. */
static const char *sniff_unknown(const uint8_t *d, size_t n, bool scriptable)
{
    if (scriptable) {
        for (size_t i = 0; i < sizeof html_sigs / sizeof html_sigs[0]; i++)
            if (match_html(d, n, html_sigs[i]))
                return "text/html";
        static const SniffPattern xml = P("<?xml", "\xFF\xFF\xFF\xFF\xFF", true, "text/xml");
        if (match(d, n, &xml))
            return "text/xml";
    }
    for (size_t i = 0; i < sizeof other_patterns / sizeof other_patterns[0]; i++) {
        const SniffPattern *p =&other_patterns[i];
        if (!scriptable && (!strcmp(p->type, "application/pdf") || !strcmp(p->type, "application/postscript")))
            continue;
        if (match(d, n, p))
            return p->type;
    }
    const char *img = sniff_image(d, n);
    if (img)
        return img;
    for (size_t i = 0; i < n; i++)
        if (binary_byte(d[i]))
            return "application/octet-stream";
    return "text/plain";
}

bool au_mime_is_javascript(const char *m)
{
    static const char *const js[] = {"application/ecmascript", "application/javascript", "application/x-ecmascript",
                                     "application/x-javascript", "text/ecmascript", "text/javascript",
                                     "text/javascript1.0", "text/javascript1.1", "text/javascript1.2",
                                     "text/javascript1.3", "text/javascript1.4", "text/javascript1.5",
                                     "text/jscript", "text/livescript", "text/x-ecmascript", "text/x-javascript"};
    for (size_t i = 0; i < sizeof js / sizeof js[0]; i++)
        if (!strcmp(m, js[i]))
            return true;
    return false;
}

static bool is_xml_or_html(const char *m)
{
    size_t n = strlen(m);
    return !strcmp(m, "text/html") || !strcmp(m, "text/xml") || !strcmp(m, "application/xml") ||
           (n > 4 && !strcmp(m + n - 4, "+xml"));
}

const char *au_mime_sniff(const char *supplied, bool nosniff, const uint8_t *data, size_t len, AuRequestDest dest)
{
    if (len > AU_SNIFF_BYTES)
        len = AU_SNIFF_BYTES;
    bool unknown = !supplied || !supplied[0] || !strcmp(supplied, "unknown/unknown") ||
                   !strcmp(supplied, "application/unknown") || !strcmp(supplied, "*/*");
    if (dest == AU_DEST_IMAGE) {
        if (nosniff && !unknown)
            return supplied;
        const char *img = sniff_image(data, len);
        if (img)
            return img;
        return unknown ? "application/octet-stream" : supplied;
    }
    if (dest == AU_DEST_DOCUMENT || dest == AU_DEST_IFRAME) {
        if (unknown)
            return sniff_unknown(data, len, !nosniff);
        if (nosniff || is_xml_or_html(supplied))
            return supplied;
        /* text/plain from servers that label everything so: the
         * "distinguishing text or binary" rules. */
        if (!strcmp(supplied, "text/plain")) {
            static const SniffPattern boms[] = {P("\xFE\xFF", "\xFF\xFF", false, ""), P("\xFF\xFE", "\xFF\xFF", false, ""),
                                           P("\xEF\xBB\xBF", "\xFF\xFF\xFF", false, "")};
            for (int i = 0; i < 3; i++)
                if (match(data, len, &boms[i]))
                    return "text/plain";
            for (size_t i = 0; i < len; i++)
                if (binary_byte(data[i]))
                    return sniff_unknown(data, len, false);
            return "text/plain";
        }
        const char *img = sniff_image(data, len);
        if (img && !strncmp(supplied, "image/", 6))
            return img;
        return supplied;
    }
    /* Style sheets, scripts, fonts, fetch: no sniffing. */
    return unknown ? "" : supplied;
}

bool au_mime_allowed(const char *mime, bool nosniff, AuRequestDest dest)
{
    if (!nosniff)
        return true;
    if (dest == AU_DEST_STYLE)
        return !strcmp(mime, "text/css");
    if (dest == AU_DEST_SCRIPT)
        return au_mime_is_javascript(mime);
    return true;
}

/* ---- Mixed content ---- */

AuMixedClass au_mixed_classify(const AuUrl *document, const AuUrl *target, AuRequestDest dest)
{
    if (!document || strcmp(document->scheme, "https") != 0)
        return AU_MIXED_NONE;
    if (au_url_trustworthy(target) || !strcmp(target->scheme, "data") || !strcmp(target->scheme, "blob") ||
        !strcmp(target->scheme, "about"))
        return AU_MIXED_NONE;
    return dest == AU_DEST_IMAGE ? AU_MIXED_UPGRADEABLE : AU_MIXED_BLOCKABLE;
}

void au_url_upgrade_to_https(AuUrl *u)
{
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "ws"))
        return;
    bool ws = !strcmp(u->scheme, "ws");
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, ws ? "wss://" : "https://");
    if (u->username[0] || u->password[0]) {
        au_buf_puts(&b, u->username);
        if (u->password[0]) {
            au_buf_putc(&b, ':');
            au_buf_puts(&b, u->password);
        }
        au_buf_putc(&b, '@');
    }
    au_buf_puts(&b, u->host);
    if (u->port >= 0 && u->port != 80)
        au_buf_printf(&b, ":%d", u->port);
    au_buf_puts(&b, u->path);
    if (u->query) {
        au_buf_putc(&b, '?');
        au_buf_puts(&b, u->query);
    }
    if (u->fragment) {
        au_buf_putc(&b, '#');
        au_buf_puts(&b, u->fragment);
    }
    AuUrl nu;
    if (au_url_parse(&nu, au_buf_cstr(&b), NULL)) {
        au_url_free(u);
        *u = nu;
    }
    au_buf_free(&b);
}
