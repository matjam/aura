/* The size of an image file from its header, for <img> elements script
 * loads (kept apart from natives2.c: src/image's headers and src/css's
 * both define AuColor). */
#include "js_int.h"

#include "../image/image.h"

bool js_image_size(const void *data, size_t len, int *w, int *h)
{
    AuImageInfo info;
    if (!au_image_probe(data, len, &info))
        return false;
    *w = info.width;
    *h = info.height;
    return true;
}
