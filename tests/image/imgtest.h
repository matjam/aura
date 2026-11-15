/* Shared helpers for the image tests: files, reference images (PNM/PAM,
 * as djpeg, dwebp -pam and ImageMagick write them), pixel comparison and
 * chunked decoding. Header-only; each test is one .c file. */
#ifndef AURA_IMGTEST_H
#define AURA_IMGTEST_H

#include "image/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int it_failures;

#define IT_CHECK(cond, ...)                                                                                       \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            it_failures++;                                                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

typedef struct ItRng {
    uint32_t s;
} ItRng;
static inline uint32_t it_rng(ItRng *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static inline uint8_t *it_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static inline bool it_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

/* Reads a PNM token (skipping comments). */
static inline int it_pnm_int(const uint8_t *b, size_t n, size_t *i)
{
    while (*i < n) {
        if (b[*i] == '#') {
            while (*i < n && b[*i] != '\n')
                (*i)++;
        } else if (b[*i] == ' ' || b[*i] == '\n' || b[*i] == '\r' || b[*i] == '\t') {
            (*i)++;
        } else {
            break;
        }
    }
    int v = 0;
    while (*i < n && b[*i] >= '0' && b[*i] <= '9')
        v = v * 10 + (b[(*i)++] - '0');
    return v;
}

/* Loads P5, P6 or P7 (GRAYSCALE, GRAYSCALE_ALPHA, RGB, RGB_ALPHA; maxval
 * 255 or 65535) as RGBA bytes, not premultiplied. NULL on failure. */
static inline uint8_t *it_load_pnm(const char *path, int *w, int *h)
{
    size_t n;
    uint8_t *b = it_read_file(path, &n);
    if (!b || n < 3 || b[0] != 'P') {
        free(b);
        return NULL;
    }
    int kind = b[1] - '0', ch = 0, maxv = 255;
    size_t i = 2;
    if (kind == 5 || kind == 6) {
        *w = it_pnm_int(b, n, &i);
        *h = it_pnm_int(b, n, &i);
        maxv = it_pnm_int(b, n, &i);
        ch = kind == 5 ? 1 : 3;
        i++;
    } else if (kind == 7) {
        *w = *h = 0;
        while (i < n) {
            while (i < n && (b[i] == '\n' || b[i] == ' '))
                i++;
            if (n - i >= 6 && !memcmp(b + i, "ENDHDR", 6)) {
                while (i < n && b[i] != '\n')
                    i++;
                i++;
                break;
            }
            char key[32] = {0}, val[32] = {0};
            size_t k = 0;
            while (i < n && b[i] != ' ' && b[i] != '\n' && k < 31)
                key[k++] = (char)b[i++];
            while (i < n && b[i] == ' ')
                i++;
            k = 0;
            while (i < n && b[i] != '\n' && k < 31)
                val[k++] = (char)b[i++];
            if (!strcmp(key, "WIDTH"))
                *w = atoi(val);
            else if (!strcmp(key, "HEIGHT"))
                *h = atoi(val);
            else if (!strcmp(key, "DEPTH"))
                ch = atoi(val);
            else if (!strcmp(key, "MAXVAL"))
                maxv = atoi(val);
        }
    } else {
        free(b);
        return NULL;
    }
    int bps = maxv > 255 ? 2 : 1;
    if (*w <= 0 || *h <= 0 || ch < 1 || ch > 4 || (size_t)*w * *h * ch * bps > n - i) {
        free(b);
        return NULL;
    }
    uint8_t *out = malloc((size_t)*w * *h * 4);
    const uint8_t *p = b + i;
    for (size_t k = 0; k < (size_t)*w * *h; k++) {
        int v[4];
        for (int c = 0; c < ch; c++) {
            v[c] = bps == 2 ? (p[0] << 8 | p[1]) * 255 / maxv : p[0] * 255 / maxv;
            p += bps;
        }
        uint8_t *o = out + k * 4;
        if (ch <= 2) {
            o[0] = o[1] = o[2] = (uint8_t)v[0];
            o[3] = ch == 2 ? (uint8_t)v[1] : 255;
        } else {
            o[0] = (uint8_t)v[0];
            o[1] = (uint8_t)v[1];
            o[2] = (uint8_t)v[2];
            o[3] = ch == 4 ? (uint8_t)v[3] : 255;
        }
    }
    free(b);
    return out;
}

/* Compares a decoded surface (premultiplied) with reference RGBA (not
 * premultiplied), in premultiplied terms. Passes when no channel differs
 * by more than max_tol and the mean absolute difference is at most
 * mean_tol. Prints the figures when it fails (or always with verbose). */
static inline bool it_compare(const AuSurface *s, const uint8_t *ref, int w, int h, int max_tol, double mean_tol,
                              const char *name)
{
    if (!s || s->width != w || s->height != h) {
        printf("FAIL %s: size %dx%d, reference %dx%d\n", name, s ? s->width : -1, s ? s->height : -1, w, h);
        it_failures++;
        return false;
    }
    int maxd = 0, mx = 0, my = 0;
    double sum = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = s->pixels[(size_t)y * s->stride + x];
            const uint8_t *r = ref + ((size_t)y * w + x) * 4;
            int a = r[3];
            int rv[4] = {a, (r[0] * a + 127) / 255, (r[1] * a + 127) / 255, (r[2] * a + 127) / 255};
            int pv[4] = {(int)(p >> 24), (int)(p >> 16 & 255), (int)(p >> 8 & 255), (int)(p & 255)};
            for (int c = 0; c < 4; c++) {
                int dd = abs(rv[c] - pv[c]);
                sum += dd;
                if (dd > maxd) {
                    maxd = dd;
                    mx = x;
                    my = y;
                }
            }
        }
    double mean = sum / ((double)w * h * 4);
    bool ok = maxd <= max_tol && mean <= mean_tol;
    if (!ok) {
        printf("FAIL %s: max diff %d at %d,%d (tolerance %d), mean %.3f (tolerance %.3f)\n", name, maxd, mx, my,
               max_tol, mean, mean_tol);
        it_failures++;
    }
    return ok;
}

/* Decodes the first frame, feeding chunks of random size (1..maxchunk;
 * maxchunk 1 feeds one byte at a time). Returns the canvas (caller frees)
 * or NULL. */
static inline AuSurface *it_decode_chunked(const uint8_t *data, size_t len, const AuImageOptions *opt,
                                           size_t maxchunk, uint32_t seed, AuImageInfo *info)
{
    AuImageOptions o = opt ? *opt : (AuImageOptions){0};
    o.first_frame_only = true;
    AuImageDecoder *d = au_image_decoder_new(&o);
    ItRng rng = {seed * 2654435761u + 1};
    size_t pos = 0;
    AuImageStatus st = AU_IMAGE_MORE;
    while (st == AU_IMAGE_MORE) {
        size_t c = maxchunk <= 1 ? 1 : 1 + it_rng(&rng) % maxchunk;
        if (c > len - pos)
            c = len - pos;
        const uint8_t *p = data + pos;
        size_t n = c;
        st = au_image_decode(d, &p, &n, pos + c == len);
        pos += c - n;
        if (st == AU_IMAGE_MORE && n) {
            printf("FAIL decoder returned MORE without consuming its input\n");
            it_failures++;
            break;
        }
    }
    AuSurface *s = NULL;
    if (st == AU_IMAGE_FRAME || (st == AU_IMAGE_ERROR && au_image_info(d)->partial))
        s = au_image_take_canvas(d);
    if (info)
        *info = *au_image_info(d);
    au_image_decoder_free(d);
    return s;
}

/* Decodes every prefix of data (every step-th length) as a complete
 * file: none may crash, hang or write out of bounds (run under ASan). */
static inline void it_truncations(const uint8_t *data, size_t len, size_t step)
{
    if (!step)
        step = 1;
    for (size_t n = 0; n < len; n += step) {
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, n, NULL, &info);
        au_surface_free(s);
    }
}

/* Decodes a whole file through every frame; returns the frame count, or
 * -1 on error before any frame. */
static inline int it_count_frames(const uint8_t *data, size_t len)
{
    AuImageDecoder *d = au_image_decoder_new(NULL);
    const uint8_t *p = data;
    size_t n = len;
    int frames = 0;
    for (int guard = 0; guard < 100000; guard++) {
        AuImageStatus st = au_image_decode(d, &p, &n, true);
        if (st == AU_IMAGE_FRAME) {
            frames++;
            continue;
        }
        if (st == AU_IMAGE_ERROR && !frames)
            frames = -1;
        break;
    }
    au_image_decoder_free(d);
    return frames;
}

#endif
