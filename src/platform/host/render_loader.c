/* A file: loader for the document view on the host. */
#include "host.h"
#include "render.h"

#include <stdio.h>
#include <string.h>

typedef struct HostReq {
    uint32_t id;
    char *url;
    bool cancelled;
} HostReq;

struct AuHostLoader {
    HostReq *q;
    uint32_t n, cap;
    uint32_t next_id;
};

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool au_host_file_path(const char *url, char *out, size_t cap)
{
    if (strncmp(url, "file://", 7) != 0)
        return false;
    const char *p = url + 7;
    /* file://host/path: only the empty host and localhost. */
    if (strncmp(p, "localhost/", 10) == 0)
        p += 9;
    if (*p != '/')
        return false;
    size_t n = 0;
    for (; *p && *p != '?' && *p != '#'; p++) {
        int c = (unsigned char)*p;
        if (c == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            c = hexval(p[1]) * 16 + hexval(p[2]);
            p += 2;
        }
        if (n + 1 >= cap)
            return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
}

static uint32_t req_fn(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    (void)dv;
    (void)kind;
    AuHostLoader *l = ctx;
    if (strncmp(url, "file://", 7) != 0)
        return 0; /* network comes later */
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->q = au_realloc(l->q, l->cap * sizeof *l->q);
    }
    HostReq *r = &l->q[l->n++];
    r->id = ++l->next_id;
    r->url = au_strndup(url, strlen(url));
    r->cancelled = false;
    return r->id;
}

static void cancel_fn(void *ctx, uint32_t id)
{
    AuHostLoader *l = ctx;
    for (uint32_t i = 0; i < l->n; i++)
        if (l->q[i].id == id)
            l->q[i].cancelled = true;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* Image dimensions from the file header. No pixels: the host has no image
 * decoders yet. */
static bool decode_fn(void *ctx, const void *data, size_t len, int *w, int *h, AuSurface **pixels)
{
    (void)ctx;
    const uint8_t *d = data;
    *pixels = NULL;
    if (len >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        *w = (int)be32(d + 16);
        *h = (int)be32(d + 20);
        return true;
    }
    if (len >= 10 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) {
        *w = (int)le16(d + 6);
        *h = (int)le16(d + 8);
        return true;
    }
    if (len >= 26 && d[0] == 'B' && d[1] == 'M') {
        *w = (int)(d[18] | (d[19] << 8) | (d[20] << 16));
        int hh = (int)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24));
        *h = hh < 0 ? -hh : hh;
        return true;
    }
    if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < len) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            size_t seg = be16(d + i + 2);
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = (int)be16(d + i + 5);
                *w = (int)be16(d + i + 7);
                return true;
            }
            i += 2 + seg;
        }
        return false;
    }
    if (len >= 5 && (!memcmp(d, "<svg", 4) || !memcmp(d, "<?xml", 5))) {
        *w = 300;
        *h = 150;
        return true;
    }
    return false;
}

AuHostLoader *au_host_loader_new(void)
{
    return au_calloc(sizeof(AuHostLoader));
}

void au_host_loader_free(AuHostLoader *l)
{
    if (!l)
        return;
    for (uint32_t i = 0; i < l->n; i++)
        au_free(l->q[i].url);
    au_free(l->q);
    au_free(l);
}

void au_host_loader_interface(AuHostLoader *l, AuLoader *out)
{
    out->ctx = l;
    out->request = req_fn;
    out->cancel = cancel_fn;
    out->decode_image = decode_fn;
}

bool au_host_loader_pump(AuHostLoader *l, AuDocView *dv)
{
    if (!l->n)
        return false;
    /* Take the queue: delivering may queue more. */
    HostReq *q = l->q;
    uint32_t n = l->n;
    l->q = NULL;
    l->n = l->cap = 0;
    for (uint32_t i = 0; i < n; i++) {
        HostReq *r = &q[i];
        if (!r->cancelled) {
            char path[4096];
            char *data = NULL;
            size_t len = 0;
            bool ok = au_host_file_path(r->url, path, sizeof path) && au_platform_read_file(path, &data, &len);
            if (ok)
                au_docview_resource_data(dv, r->id, data, len);
            au_docview_resource_done(dv, r->id, ok);
            au_free(data);
        }
        au_free(r->url);
    }
    au_free(q);
    return true;
}
