/* Layout queries for nested documents (src/page/frames.c). */
#include "layout_int.h"

bool au_layout_element_content_rect(AuLayout *L, const AuElement *el, AuRect *out)
{
    (void)L;
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!b || b->kind != LK_BLOCK)
        return false;
    LBlock *k = (LBlock *)b;
    float x, y;
    l_box_origin(b, &x, &y);
    out->x = x + k->bw[3] + k->p[3];
    out->y = y + k->bw[0] + k->p[0];
    out->w = k->w - k->bw[1] - k->bw[3] - k->p[1] - k->p[3];
    out->h = k->h - k->bw[0] - k->bw[2] - k->p[0] - k->p[2];
    if (out->w < 0)
        out->w = 0;
    if (out->h < 0)
        out->h = 0;
    return true;
}

void au_layout_set_transparent(AuLayout *L, bool on)
{
    if (L->transparent != on) {
        L->transparent = on;
        L->need_paint = true;
    }
}
