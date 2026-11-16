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

bool au_layout_css_container_size(void *layout, const AuElement *el, float *w, float *h)
{
    (void)layout;
    LBox *b = el ? (LBox *)el->box : NULL;
    if (!b || b->kind != LK_BLOCK || (b->flags & (LF_DEAD | LF_HAS_LAYOUT)) != LF_HAS_LAYOUT)
        return false;
    LBlock *k = (LBlock *)b;
    *w = l_max(0, k->w - k->bw[1] - k->bw[3] - k->p[1] - k->p[3]);
    *h = l_max(0, k->h - k->bw[0] - k->bw[2] - k->p[0] - k->p[2]);
    return true;
}

void au_layout_set_transparent(AuLayout *L, bool on)
{
    if (L->transparent != on) {
        L->transparent = on;
        L->need_paint = true;
    }
}
