/* Checks over a folder of vectors with .pam references, shared by the
 * PNG, GIF and BMP tests:
 *
 *   NAME.EXT + NAME.pam             a still image
 *   NAME.EXT + NAME.fN.pam + .txt   an animation ("frames N loop L delays D...") */
#ifndef AURA_VECTEST_H
#define AURA_VECTEST_H

#include "imgtest.h"

#include <dirent.h>

static char vt_dir[1024];

static uint8_t *vt_load(const char *name, size_t *len)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", vt_dir, name);
    return it_read_file(path, len);
}

static uint8_t *vt_load_ref(const char *name, int *w, int *h)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", vt_dir, name);
    return it_load_pnm(path, w, h);
}

static inline bool vt_same_pixels(const AuSurface *a, const AuSurface *b)
{
    if (!a || !b || a->width != b->width || a->height != b->height)
        return false;
    for (int y = 0; y < a->height; y++)
        if (memcmp(a->pixels + (size_t)y * a->stride, b->pixels + (size_t)y * b->stride, (size_t)a->width * 4))
            return false;
    return true;
}

/* Every NAME.ext with a NAME.pam: matches within the tolerance, decodes
 * the same whole, a byte at a time and in random chunks, has the right
 * opaque flag, and survives every truncation. Returns the count. */
static inline int vt_stills(const char *ext, AuImageFormat fmt, int max_tol, double mean_tol)
{
    DIR *dir = opendir(vt_dir);
    IT_CHECK(dir != NULL, "cannot open %s", vt_dir);
    if (!dir)
        return 0;
    struct dirent *e;
    int n = 0;
    size_t el = strlen(ext);
    while ((e = readdir(dir))) {
        size_t nl = strlen(e->d_name);
        if (nl <= el + 1 || strcmp(e->d_name + nl - el, ext) || e->d_name[nl - el - 1] != '.')
            continue;
        char base[256], refname[300];
        snprintf(base, sizeof base, "%.*s", (int)(nl - el - 1), e->d_name);
        snprintf(refname, sizeof refname, "%s.pam", base);
        int rw, rh;
        uint8_t *ref = vt_load_ref(refname, &rw, &rh);
        if (!ref)
            continue; /* an animation, or a vector with its own test */
        size_t len;
        uint8_t *data = vt_load(e->d_name, &len);
        IT_CHECK(data != NULL, "%s: cannot read", base);
        if (!data) {
            free(ref);
            continue;
        }
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
        IT_CHECK(s != NULL, "%s: decode failed (%s)", base, info.ready ? "after the header" : "no header");
        if (s) {
            it_compare(s, ref, rw, rh, max_tol, mean_tol, base);
            IT_CHECK(info.format == fmt, "%s: format %s", base, au_image_format_name(info.format));
            AuSurface *c1 = it_decode_chunked(data, len, NULL, 1, 1, NULL);
            IT_CHECK(vt_same_pixels(s, c1), "%s: byte-at-a-time decode differs", base);
            AuSurface *c2 = it_decode_chunked(data, len, NULL, 50, (uint32_t)n + 3, NULL);
            IT_CHECK(vt_same_pixels(s, c2), "%s: chunked decode differs", base);
            au_surface_free(c1);
            au_surface_free(c2);
            bool opaque = true;
            for (size_t i = 0; i < (size_t)rw * rh; i++)
                opaque &= ref[i * 4 + 3] == 255;
            IT_CHECK(s->opaque == opaque, "%s: opaque flag %d, expected %d", base, s->opaque, opaque);
        }
        it_truncations(data, len, len > 4000 ? 7 : 1);
        au_surface_free(s);
        free(data);
        free(ref);
        n++;
    }
    closedir(dir);
    return n;
}

/* An animation: every frame against its reference, delays (as given,
 * before the minimum rule), loop count, frame count, whole and in
 * 3-byte chunks; truncations. */
static inline void vt_animation(const char *name, const char *ext, int max_tol)
{
    char fname[300], txt[300];
    snprintf(fname, sizeof fname, "%s.%s", name, ext);
    snprintf(txt, sizeof txt, "%s.txt", name);
    size_t len, tl;
    uint8_t *data = vt_load(fname, &len);
    uint8_t *meta = vt_load(txt, &tl);
    IT_CHECK(data && meta, "%s: missing", name);
    if (!data || !meta) {
        free(data);
        free(meta);
        return;
    }
    char mbuf[512];
    snprintf(mbuf, sizeof mbuf, "%.*s", (int)(tl < 500 ? tl : 500), (char *)meta);
    int frames = 0, loop = 0, delays[64], nd = 0;
    char *p = mbuf;
    sscanf(p, "frames %d loop %d delays", &frames, &loop);
    p = strstr(p, "delays");
    if (p) {
        p += 6;
        int off;
        while (nd < 64 && sscanf(p, "%d%n", &delays[nd], &off) == 1) {
            nd++;
            p += off;
        }
    }
    for (int chunked = 0; chunked < 2; chunked++) {
        AuImageDecoder *d = au_image_decoder_new(NULL);
        const uint8_t *in = data;
        size_t left = len;
        int f = 0;
        for (int guard = 0; guard < 1000000; guard++) {
            size_t c = chunked ? (left < 3 ? left : 3) : left;
            const uint8_t *q = in;
            size_t qn = c;
            AuImageStatus st = au_image_decode(d, &q, &qn, c == left);
            in += c - qn;
            left -= c - qn;
            if (st == AU_IMAGE_FRAME) {
                char rn[300], label[300];
                snprintf(rn, sizeof rn, "%s.f%d.pam", name, f);
                snprintf(label, sizeof label, "%s frame %d%s", name, f, chunked ? " (chunked)" : "");
                int rw, rh;
                uint8_t *ref = vt_load_ref(rn, &rw, &rh);
                IT_CHECK(ref != NULL, "%s: no reference", label);
                if (ref)
                    it_compare(au_image_canvas(d), ref, rw, rh, max_tol, max_tol ? 0.5 : 0, label);
                free(ref);
                const AuImageInfo *info = au_image_info(d);
                IT_CHECK(f >= nd || info->frame_delay_ms == (uint32_t)delays[f], "%s: delay %u, expected %d", label,
                         info->frame_delay_ms, f < nd ? delays[f] : -1);
                IT_CHECK(info->loop_count == loop, "%s: loop %d, expected %d", label, info->loop_count, loop);
                f++;
                continue;
            }
            if (st == AU_IMAGE_MORE)
                continue;
            IT_CHECK(st == AU_IMAGE_DONE, "%s: ended with %d (%s)", name, st, au_image_error(d));
            break;
        }
        IT_CHECK(f == frames, "%s: %d frames, expected %d", name, f, frames);
        IT_CHECK(au_image_info(d)->animated == (frames > 1), "%s: animated flag", name);
        au_image_decoder_free(d);
    }
    it_truncations(data, len, 1);
    free(data);
    free(meta);
}

/* Box-averages a premultiplied surface by f, rounding as the decoder
 * does: the reference for scaled decoding of non-JPEG formats. */
static inline AuSurface *vt_box_reduce(const AuSurface *s, int f)
{
    int ow = (s->width + f - 1) / f, oh = (s->height + f - 1) / f;
    AuSurface *o = au_surface_new(ow, oh);
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++) {
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (int y = oy * f; y < oy * f + f && y < s->height; y++)
                for (int x = ox * f; x < ox * f + f && x < s->width; x++) {
                    uint32_t px = s->pixels[(size_t)y * s->stride + x];
                    sum[0] += px >> 24;
                    sum[1] += px >> 16 & 255;
                    sum[2] += px >> 8 & 255;
                    sum[3] += px & 255;
                    n++;
                }
            uint32_t v[4];
            for (int c = 0; c < 4; c++)
                v[c] = (sum[c] + n / 2) / n;
            o->pixels[(size_t)oy * o->stride + ox] = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
        }
    return o;
}

#endif
