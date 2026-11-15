/* WebP: not written yet. */
#include "image_int.h"

static void *create(AuImageDecoder *d) { (void)d; return au_calloc(1); }
static AuImageStatus run(void *st, AuImageDecoder *d, const uint8_t **in, size_t *len, bool final)
{
    (void)st; (void)in; (void)len; (void)final;
    return au_img_fail(d, "unsupported");
}
static void destroy(void *st) { au_free(st); }
const AuImgCodec au_img_webp_codec = {create, run, destroy};
