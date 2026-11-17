/* SVG as an image: a codec that holds the bytes until the last one, then
 * parses the document with src/svg and rasterizes it.
 *
 * The intrinsic size (info.width and height) is the size an <img> shows
 * the document at without CSS sizes (au_svg_doc_default_size: its width
 * and height, one from the other through the viewBox ratio, else 300 x
 * 150). info.missing says which of a natural width, height and ratio
 * the document lacks (only a viewBox: width and height), and layout
 * sizes the image from its box instead. The canvas is not a reduction of it: it is the target size when
 * one is given (both sides, or one side and the intrinsic ratio), so the
 * vector image is drawn sharp at the size it is shown, shrunk to fit
 * max_bytes. scale_denom stays 1. The document loads nothing: only
 * data: URLs in <image> draw, and scripts never run. */
#include "image_int.h"

#include "../svg/svg.h"

#include <math.h>

typedef struct SvgImg {
    AuBuf buf;
    bool root_ok; /* the root element is svg */
} SvgImg;

static void *svgimg_create(AuImageDecoder *d)
{
    (void)d;
    SvgImg *s = au_calloc(sizeof *s);
    au_buf_init(&s->buf);
    return s;
}

static void svgimg_destroy(void *st)
{
    SvgImg *s = st;
    au_buf_free(&s->buf);
    au_free(s);
}

/* The canvas size for the target and the cap. */
static void raster_size(const AuImageDecoder *d, int w, int h, int *ow, int *oh)
{
    float tw = (float)d->opt.target_w, th = (float)d->opt.target_h;
    if (tw <= 0 && th <= 0)
        tw = (float)w, th = (float)h;
    else if (th <= 0)
        th = tw * (float)h / (float)w;
    else if (tw <= 0)
        tw = th * (float)w / (float)h;
    if (tw > AU_IMAGE_MAX_DIM)
        th *= AU_IMAGE_MAX_DIM / tw, tw = AU_IMAGE_MAX_DIM;
    if (th > AU_IMAGE_MAX_DIM)
        tw *= AU_IMAGE_MAX_DIM / th, th = AU_IMAGE_MAX_DIM;
    double bytes = (double)tw * (double)th * 4;
    if (bytes > (double)d->opt.max_bytes) {
        float k = (float)sqrt((double)d->opt.max_bytes / bytes);
        tw *= k;
        th *= k;
    }
    *ow = tw < 1 ? 1 : au_f2i(tw + 0.5f);
    *oh = th < 1 ? 1 : au_f2i(th + 0.5f);
    if (*ow < 1)
        *ow = 1; /* not a number */
    if (*oh < 1)
        *oh = 1;
    while ((size_t)*ow * (size_t)*oh * 4 > d->opt.max_bytes && (*ow > 1 || *oh > 1)) {
        if (*ow > 1)
            (*ow)--;
        if (*oh > 1)
            (*oh)--;
    }
}

/* Whether the bytes so far start an SVG document: 1 yes, -1 no (another
 * root element or doctype: an HTML error page, say), 0 not known yet.
 * Skips a BOM, whitespace, comments and processing instructions. */
static int root_state(const char *s, size_t len)
{
    const char *e = s + (len > 65536 ? 65536 : len);
    if (e - s >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
        s += 3;
    for (;;) {
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        if (s >= e)
            return len > 65536 ? -1 : 0;
        if (*s != '<')
            return -1;
        if (e - s < 10)
            return 0;
        const char *end = NULL;
        if (!memcmp(s, "<!--", 4)) {
            for (const char *p = s + 4; p + 3 <= e && !end; p++)
                if (!memcmp(p, "-->", 3))
                    end = p + 3;
        } else if (s[1] == '?') {
            for (const char *p = s + 2; p + 2 <= e && !end; p++)
                if (!memcmp(p, "?>", 2))
                    end = p + 2;
        } else if (!memcmp(s, "<!DOCTYPE", 9)) {
            const char *p = s + 9;
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;
            if (e - p < 4)
                return 0;
            return !memcmp(p, "svg", 3) && !((p[3] >= 'a' && p[3] <= 'z') || (p[3] >= 'A' && p[3] <= 'Z')) ? 1 : -1;
        } else {
            /* The root element's name, prefix allowed (svg:svg). */
            const char *p = s + 1, *colon = NULL;
            while (p < e && *p != ' ' && *p != '>' && *p != '/' && *p != '\t' && *p != '\n' && *p != '\r') {
                if (*p == ':')
                    colon = p;
                p++;
            }
            if (p >= e)
                return 0;
            const char *name = colon ? colon + 1 : s + 1;
            return p - name == 3 && !memcmp(name, "svg", 3) ? 1 : -1;
        }
        if (!end)
            return len > 65536 ? -1 : 0;
        s = end;
    }
}

static AuImageStatus svgimg_run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    SvgImg *s = st;
    if (d->info.frame_count)
        return AU_IMAGE_DONE;
    if (*len) {
        if (s->buf.len + *len > AU_SVG_MAX_BYTES)
            return au_img_fail(d, "SVG too large");
        au_buf_append(&s->buf, *in, *len);
        au_img_mem(d, (ptrdiff_t)*len);
        *in += *len;
        *len = 0;
    }
    if (!s->root_ok) {
        int r = root_state(s->buf.data ? s->buf.data : "", s->buf.len);
        if (r < 0)
            return au_img_fail(d, "not an SVG document");
        s->root_ok = r > 0;
    }
    if (!final)
        return AU_IMAGE_MORE;
    AuSvgDoc *doc = au_svg_doc_new(s->buf.data, s->buf.len, 0);
    if (!doc)
        return au_img_fail(d, "not an SVG document");
    int w, h;
    au_svg_doc_default_size(doc, &w, &h);
    /* The limits of au_img_header, without its reduction. */
    if ((uint64_t)w * (uint64_t)h > d->opt.max_source_pixels) {
        au_svg_doc_free(doc);
        return au_img_fail(d, "image has too many pixels");
    }
    d->info.width = w;
    d->info.height = h;
    AuSvgIntrinsic nat;
    au_svg_doc_intrinsic(doc, &nat);
    d->info.missing = (uint8_t)((nat.width > 0 ? 0 : AU_IMAGE_MISSING_W) | (nat.height > 0 ? 0 : AU_IMAGE_MISSING_H) |
                                (nat.ratio > 0 || (nat.width > 0 && nat.height > 0) ? 0 : AU_IMAGE_MISSING_RATIO));
    d->info.has_alpha = true;
    d->info.orientation = 1;
    d->info.scale_denom = 1;
    raster_size(d, w, h, &d->info.out_w, &d->info.out_h);
    d->info.ready = true;
    if (d->opt.header_only) {
        au_svg_doc_free(doc);
        return AU_IMAGE_DONE;
    }
    AuSurface *c = au_svg_doc_rasterize(doc, d->info.out_w, d->info.out_h, d->opt.fonts);
    au_svg_doc_free(doc);
    if (!c)
        return au_img_fail(d, "SVG could not be drawn");
    d->canvas = c;
    d->cw = c->width;
    d->ch = c->height;
    d->next_row = c->height;
    d->any_dirty = true;
    d->dirty = (AuIRect){0, 0, c->width, c->height};
    au_img_frame_done(d, 0);
    return AU_IMAGE_FRAME;
}

const AuImgCodec au_img_svg_codec = {svgimg_create, svgimg_run, svgimg_destroy};
