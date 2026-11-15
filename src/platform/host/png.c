/* PNG writer and stored-block reader. See png.h. */
#include "png.h"

#include <stdio.h>
#include <string.h>

static void put32(AuBuf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    au_buf_append(b, x, 4);
}

static void chunk(AuBuf *out, const char *type, const uint8_t *data, size_t n)
{
    put32(out, (uint32_t)n);
    size_t start = out->len;
    au_buf_append(out, type, 4);
    if (n)
        au_buf_append(out, data, n);
    put32(out, au_crc32(0, (const uint8_t *)out->data + start, n + 4));
}

void au_png_encode(const AuSurface *s, AuBuf *out)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    au_buf_append(out, sig, 8);
    uint8_t ihdr[13] = {0};
    for (int i = 0; i < 4; i++) {
        ihdr[i] = (uint8_t)((uint32_t)s->width >> (24 - 8 * i));
        ihdr[4 + i] = (uint8_t)((uint32_t)s->height >> (24 - 8 * i));
    }
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    chunk(out, "IHDR", ihdr, 13);

    /* Raw image: a filter byte (0, none) then RGBA per row. */
    size_t rowlen = (size_t)s->width * 4 + 1;
    size_t rawlen = rowlen * (size_t)s->height;
    uint8_t *raw = au_alloc(rawlen ? rawlen : 1);
    for (int y = 0; y < s->height; y++) {
        uint8_t *r = raw + (size_t)y * rowlen;
        *r++ = 0;
        for (int x = 0; x < s->width; x++) {
            AuColor c = au_surface_get(s, x, y);
            *r++ = (uint8_t)(c >> 16);
            *r++ = (uint8_t)(c >> 8);
            *r++ = (uint8_t)c;
            *r++ = (uint8_t)(c >> 24);
        }
    }
    /* zlib: header, stored blocks of up to 65535 bytes, Adler-32. */
    AuBuf z;
    au_buf_init(&z);
    au_buf_putc(&z, 0x78);
    au_buf_putc(&z, 0x01);
    size_t pos = 0;
    do {
        size_t n = rawlen - pos < 65535 ? rawlen - pos : 65535;
        bool last = pos + n == rawlen;
        uint8_t hdr[5] = {(uint8_t)(last ? 1 : 0), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)};
        au_buf_append(&z, hdr, 5);
        au_buf_append(&z, raw + pos, n);
        pos += n;
    } while (pos < rawlen);
    put32(&z, au_adler32(1, raw, rawlen));
    chunk(out, "IDAT", (const uint8_t *)z.data, z.len);
    chunk(out, "IEND", NULL, 0);
    au_buf_free(&z);
    au_free(raw);
}

bool au_png_write(const char *path, const AuSurface *s)
{
    AuBuf b;
    au_buf_init(&b);
    au_png_encode(s, &b);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(b.data, 1, b.len, f) == b.len;
    if (f && fclose(f) != 0)
        ok = false;
    au_buf_free(&b);
    return ok;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

AuSurface *au_png_read_stored(const uint8_t *data, size_t len)
{
    if (len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0)
        return NULL;
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    AuBuf z;
    au_buf_init(&z);
    while (pos + 12 <= len) {
        uint32_t n = get32(data + pos);
        if (n > len - pos - 12)
            break;
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (!memcmp(type, "IHDR", 4) && n == 13) {
            w = get32(body);
            h = get32(body + 4);
            if (body[8] != 8 || body[9] != 6 || body[12] != 0)
                goto bad;
        } else if (!memcmp(type, "IDAT", 4)) {
            au_buf_append(&z, body, n);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + n;
    }
    if (!w || !h || w > 16384 || h > 16384 || z.len < 2)
        goto bad;
    size_t rowlen = (size_t)w * 4 + 1, rawlen = rowlen * h;
    uint8_t *raw = au_alloc(rawlen);
    size_t got = 0, zp = 2;
    const uint8_t *zd = (const uint8_t *)z.data;
    for (;;) {
        if (zp + 5 > z.len || (zd[zp] & 6) != 0) /* stored blocks only */
            break;
        bool last = zd[zp] & 1;
        size_t n = zd[zp + 1] | ((size_t)zd[zp + 2] << 8);
        zp += 5;
        if (n > z.len - zp || n > rawlen - got)
            break;
        memcpy(raw + got, zd + zp, n);
        got += n;
        zp += n;
        if (last)
            break;
    }
    if (got != rawlen) {
        au_free(raw);
        goto bad;
    }
    AuSurface *s = au_surface_new((int)w, (int)h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = raw + y * rowlen;
        if (r[0] != 0) {
            au_free(raw);
            au_surface_free(s);
            goto bad;
        }
        r++;
        for (uint32_t x = 0; x < w; x++, r += 4) {
            uint32_t a = r[3];
            uint32_t R = (r[0] * a + 127) / 255, G = (r[1] * a + 127) / 255, B = (r[2] * a + 127) / 255;
            s->pixels[y * s->stride + x] = (a << 24) | (R << 16) | (G << 8) | B;
        }
    }
    au_free(raw);
    au_buf_free(&z);
    return s;
bad:
    au_buf_free(&z);
    return NULL;
}
