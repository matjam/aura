/* BMP and ICO: not written yet. */
#include "image_int.h"

static void *create(AuImageDecoder *d) { (void)d; return au_calloc(1); }
static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    (void)st; (void)in; (void)len; (void)final;
    return au_img_fail(d, "unsupported");
}
static void destroy(void *st) { au_free(st); }
const AuImgCodec au_img_bmp_codec = {create, run, destroy};
const AuImgCodec au_img_ico_codec = {create, run, destroy};

AuSurface *au_image_decode_favicon(const void *data, size_t len, int size)
{
    (void)data; (void)len; (void)size;
    return NULL;
}
