/* Named character reference lookup over the packed table in
 * entities_data.h (see gen_entities.py for its layout). */
#include "html_internal.h"

#include <string.h>

#include "entities_data.h"

int au_html_entity_find(const char *name, size_t len)
{
    int lo = 0, hi = ENTITY_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = entity_names + entity_off[mid];
        size_t slen = (size_t)(entity_off[mid + 1] - entity_off[mid]);
        size_t n = len < slen ? len : slen;
        int c = memcmp(name, s, n);
        if (c == 0)
            c = (len > slen) - (len < slen);
        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

bool au_html_entity_legacy(int idx)
{
    return (entity_val[idx] >> 21) & 1;
}

int au_html_entity_codepoints(int idx, uint32_t cp[2])
{
    uint32_t v = entity_val[idx];
    cp[0] = v & 0x1FFFF;
    uint32_t i = (v >> 17) & 15;
    if (!i)
        return 1;
    cp[1] = entity_cp2[i];
    return 2;
}

size_t au_html_entity_max_len(void)
{
    return ENTITY_MAX_LEN;
}
