/* SVG documents: files, data: URLs and SVG used as an image.
 *
 * A document is parsed by xml.c and styled by its own resolver with the
 * sheets of its <style> elements (imports are never fetched). It loads
 * nothing: an <image> draws only a data: URL, decoded once per element
 * (a nested SVG is re-rasterized when it is wanted at another size), and
 * scripts are ignored. */
#include "svg_int.h"

#include "../image/image.h"
#include "../net/dataurl.h"

#define MAX_DOC_IMAGES 64
#define MAX_IMAGE_BYTES ((size_t)4 * 1024 * 1024)

typedef struct DocImage {
    const AuElement *el;
    AuSurface *surf;   /* decoded raster, or the nested SVG's pixels */
    AuSvgDoc *svg;     /* a nested SVG document */
    bool failed;
} DocImage;

struct AuSvgDoc {
    AuDocument *doc;
    AuCssResolver *res;
    AuSvgNames *names;
    AuElement *root;
    int depth;
    DocImage images[MAX_DOC_IMAGES];
    uint32_t nimages;
    uint32_t nodes;
};

/* ---- Sniffing ---- */

bool au_svg_sniff(const void *data, size_t len)
{
    const char *s = data, *e = s + (len > 4096 ? 4096 : len);
    if (e - s >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
        s += 3;
    for (int guard = 0; guard < 64; guard++) {
        while (s < e && svg_ws(*s))
            s++;
        if (e - s < 4 || *s != '<')
            return false;
        if (e - s >= 4 && !memcmp(s, "<svg", 4))
            return e - s == 4 || svg_ws(s[4]) || s[4] == '>' || s[4] == '/';
        if (e - s >= 9 && !memcmp(s, "<!DOCTYPE", 9)) {
            const char *p = s + 9;
            while (p < e && svg_ws(*p))
                p++;
            return e - p >= 3 && !memcmp(p, "svg", 3);
        }
        const char *end = NULL;
        if (!memcmp(s, "<!--", 4)) {
            for (const char *p = s + 4; p + 3 <= e; p++)
                if (!memcmp(p, "-->", 3)) {
                    end = p + 3;
                    break;
                }
        } else if (s[1] == '?') {
            for (const char *p = s + 2; p + 2 <= e; p++)
                if (!memcmp(p, "?>", 2)) {
                    end = p + 2;
                    break;
                }
        }
        if (!end)
            return false;
        s = end;
    }
    return false;
}

bool au_svg_data_url(const char *url, size_t max, uint8_t **out, size_t *out_len, char *mime, size_t cap)
{
    *out = NULL;
    *out_len = 0;
    if (cap)
        mime[0] = 0;
    if (!url || strncmp(url, "data:", 5) != 0)
        return false;
    AuDataUrl d;
    au_data_url_init(&d);
    if (!au_data_url_decode(url, &d) || d.body.len > max) {
        au_data_url_free(&d);
        return false;
    }
    if (cap) {
        size_t n = d.mime.len < cap - 1 ? d.mime.len : cap - 1;
        if (n)
            memcpy(mime, d.mime.data, n);
        mime[n] = 0;
    }
    *out = au_alloc(d.body.len ? d.body.len : 1);
    if (d.body.len)
        memcpy(*out, d.body.data, d.body.len);
    *out_len = d.body.len;
    au_data_url_free(&d);
    return true;
}

/* ---- Documents ---- */

static bool style_type_ok(const AuElement *el)
{
    const char *t = au_element_get_attr(el, au_atom_cstr("type"));
    return !t || !*t || !strcmp(t, "text/css");
}

AuSvgDoc *au_svg_doc_new(const void *data, size_t len, int depth)
{
    if (!data || !len || len > AU_SVG_MAX_BYTES || depth >= SVG_MAX_IMAGE_DEPTH)
        return NULL;
    AuDocument *doc = au_xml_parse(data, len, NULL, NULL);
    if (!doc)
        return NULL;
    AuSvgDoc *d = au_calloc(sizeof *d);
    d->doc = doc;
    d->depth = depth;
    d->names = au_svg_names_new();
    AuElement *root = au_document_element(doc);
    if (!root || !svg_is_tag(root, d->names->svg)) {
        au_svg_doc_free(d);
        return NULL;
    }
    d->root = root;
    d->res = au_css_resolver_new();
    AuNode *top = &doc->node;
    for (AuNode *n = top; n; n = au_node_next(n, top)) {
        if (n->type == AU_NODE_ELEMENT || n->type == AU_NODE_TEXT)
            d->nodes++;
        if (n->type != AU_NODE_ELEMENT || !svg_is_tag(au_as_element(n), d->names->style) ||
            !style_type_ok(au_as_element(n)))
            continue;
        AuBuf css;
        au_buf_init(&css);
        for (AuNode *c = n->first_child; c; c = c->next)
            if (c->type == AU_NODE_TEXT)
                au_buf_append(&css, au_as_text(c)->data, au_as_text(c)->len);
        const char *media = au_element_get_attr(au_as_element(n), d->names->media);
        au_css_resolver_add_sheet(d->res,
                                  au_css_sheet_parse(css.data ? css.data : "", css.len, AU_CSS_ORIGIN_AUTHOR, NULL, media));
        au_buf_free(&css);
    }
    au_css_style_document(d->res, doc);
    if (!au_element_style(root)) {
        au_svg_doc_free(d);
        return NULL;
    }
    return d;
}

void au_svg_doc_free(AuSvgDoc *d)
{
    if (!d)
        return;
    for (uint32_t i = 0; i < d->nimages; i++) {
        if (d->images[i].surf)
            au_surface_free(d->images[i].surf);
        au_svg_doc_free(d->images[i].svg);
    }
    if (d->doc) {
        au_css_clear_document(d->doc);
        au_node_unref(&d->doc->node);
    }
    if (d->res)
        au_css_resolver_free(d->res);
    au_svg_names_free(d->names);
    au_free(d);
}

void au_svg_doc_intrinsic(const AuSvgDoc *d, AuSvgIntrinsic *out)
{
    const AuStyle *s = au_element_style(d->root);
    au_svg_intrinsic(d->names, d->root, s ? s->inh->font_size : 16, out);
}

void au_svg_doc_default_size(const AuSvgDoc *d, int *w, int *h)
{
    AuSvgIntrinsic in;
    au_svg_doc_intrinsic(d, &in);
    float fw = in.width, fh = in.height;
    if (fw > 0 && fh > 0) {
    } else if (fw > 0 && in.ratio > 0) {
        fh = fw / in.ratio;
    } else if (fh > 0 && in.ratio > 0) {
        fw = fh * in.ratio;
    } else if (fw > 0) {
        fh = 150;
    } else if (fh > 0) {
        fw = 300;
    } else if (in.ratio > 0) {
        /* Only a ratio: 300 wide unless that is too tall, as for <img>
         * in a 300 x 150 default box. */
        fw = 300;
        fh = 300 / in.ratio;
        if (fh > 150) {
            fh = 150;
            fw = 150 * in.ratio;
        }
    } else {
        fw = 300;
        fh = 150;
    }
    /* !(x >= 1): below 1, or not a number */
    *w = !(fw >= 1) ? 1 : fw > AU_IMAGE_MAX_DIM ? AU_IMAGE_MAX_DIM : (int)(fw + 0.5f);
    *h = !(fh >= 1) ? 1 : fh > AU_IMAGE_MAX_DIM ? AU_IMAGE_MAX_DIM : (int)(fh + 0.5f);
}

/* The env's image callback: data: URLs only. */
static const AuSurface *doc_image(void *ctx, const AuElement *el, const char *href, int dw, int dh)
{
    AuSvgDoc *d = ctx;
    DocImage *im = NULL;
    for (uint32_t i = 0; i < d->nimages; i++)
        if (d->images[i].el == el)
            im = &d->images[i];
    if (!im) {
        if (d->nimages >= MAX_DOC_IMAGES)
            return NULL;
        im = &d->images[d->nimages++];
        memset(im, 0, sizeof *im);
        im->el = el;
        uint8_t *bytes;
        size_t n;
        char mime[64];
        while (svg_ws(*href))
            href++;
        if (!au_svg_data_url(href, MAX_IMAGE_BYTES, &bytes, &n, mime, sizeof mime)) {
            im->failed = true;
            return NULL;
        }
        if (!strcmp(mime, "image/svg+xml") || au_svg_sniff(bytes, n)) {
            im->svg = au_svg_doc_new(bytes, n, d->depth + 1);
        } else {
            AuImageOptions o;
            memset(&o, 0, sizeof o);
            o.first_frame_only = true;
            o.max_bytes = MAX_IMAGE_BYTES;
            AuImageInfo info;
            im->surf = au_image_decode_buffer(bytes, n, &o, &info);
        }
        au_free(bytes);
        im->failed = !im->svg && !im->surf;
    }
    if (im->failed)
        return NULL;
    if (im->svg) {
        int w = dw, h = dh;
        if (w <= 0 || h <= 0)
            au_svg_doc_default_size(im->svg, &w, &h);
        if (w > 4096)
            w = 4096;
        if (h > 4096)
            h = 4096;
        if (!im->surf || im->surf->width != w || im->surf->height != h) {
            if (im->surf)
                au_surface_free(im->surf);
            im->surf = au_svg_doc_rasterize(im->svg, w, h, NULL);
        }
    }
    return im->surf;
}

void au_svg_doc_render(AuSvgDoc *d, AuCanvas *c, AuRect view, AuFontSet *fonts)
{
    if (!d)
        return;
    AuSvgEnv env;
    memset(&env, 0, sizeof env);
    env.fonts = fonts;
    env.ctx = d;
    env.image = doc_image;
    au_canvas_clip_push(c, view);
    au_svg_paint(c, d->names, d->root, view, &env);
    au_canvas_clip_pop(c);
}

AuSurface *au_svg_doc_rasterize(AuSvgDoc *d, int w, int h, AuFontSet *fonts)
{
    if (!d || w <= 0 || h <= 0 || w > AU_IMAGE_MAX_DIM || h > AU_IMAGE_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)AU_IMAGE_DEFAULT_MAX_BYTES / 4)
        return NULL;
    AuSurface *s = au_surface_new(w, h);
    if (!s)
        return NULL;
    AuCanvas *c = au_canvas_new(s);
    au_svg_doc_render(d, c, (AuRect){0, 0, (float)w, (float)h}, fonts);
    au_canvas_free(c);
    s->opaque = false;
    return s;
}

void au_svg_doc_stats(const AuSvgDoc *d, uint32_t *nodes, size_t *bytes)
{
    *nodes = d->nodes;
    size_t b = sizeof *d;
    AuNode *top = &d->doc->node;
    for (AuNode *n = top; n; n = au_node_next(n, top)) {
        if (n->type == AU_NODE_ELEMENT) {
            const AuElement *el = au_as_element(n);
            b += sizeof(AuElement) + el->nattrs * sizeof(AuAttr);
            for (uint32_t i = 0; i < el->nattrs; i++)
                b += el->attrs[i].value ? strlen(el->attrs[i].value) + 1 : 0;
        } else if (n->type == AU_NODE_TEXT) {
            b += sizeof(AuText) + au_as_text(n)->cap;
        }
    }
    *bytes = b;
}
