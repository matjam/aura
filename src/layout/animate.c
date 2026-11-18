/* Playing CSS animations in layout (see LAnim in layout_int.h). */
#include "layout_int.h"

/* 20 frames a second: enough for a spinner, and a Mac OS 9 machine
 * repaints a small box that often without falling behind. */
#define ANIM_FRAME_MS 50

static void anim_drop(AuLayout *L, uint32_t i)
{
    LAnim *a = L->anims[i];
    if (a->k && a->k->anim == a)
        a->k->anim = NULL;
    au_free(a);
    L->anims[i] = L->anims[--L->nanims];
}

bool l_anim_register(AuLayout *L, LBlock *k)
{
    const LBox *b = &k->b;
    if (lb_anon(b) || !L->env.anim_sample || !au_css_animation_playing(b->style)) {
        if (k->anim) {
            for (uint32_t i = 0; i < L->nanims; i++)
                if (L->anims[i] == k->anim) {
                    anim_drop(L, i);
                    break;
                }
        }
        return false;
    }
    AuCssAnimSample s;
    if (!L->env.anim_sample(L->env.ctx, b->style, L->anim_ms, &s))
        memset(&s, 0, sizeof s);
    if (!k->anim) {
        if (L->nanims >= 256)
            return false;
        if (L->nanims == L->anims_cap) {
            L->anims_cap = L->anims_cap ? L->anims_cap * 2 : 8;
            L->anims = au_realloc(L->anims, L->anims_cap * sizeof *L->anims);
        }
        LAnim *a = au_calloc(sizeof *a);
        a->k = k;
        k->anim = a;
        L->anims[L->nanims++] = a;
    }
    k->anim->s = s;
    return true;
}

void l_anim_clear(AuLayout *L)
{
    for (uint32_t i = 0; i < L->nanims; i++)
        au_free(L->anims[i]);
    L->nanims = 0;
}

bool au_layout_animating(const AuLayout *L)
{
    return L && L->nanims > 0;
}

static void emit(AuRect r, void (*fn)(void *ctx, AuIRect r), void *ctx)
{
    if (!(r.w > 0 && r.h > 0))
        return;
    int x0 = (int)floorf(r.x) - 2, y0 = (int)floorf(r.y) - 2;
    int x1 = (int)ceilf(r.x + r.w) + 2, y1 = (int)ceilf(r.y + r.h) + 2;
    fn(ctx, (AuIRect){x0, y0, x1, y1});
}

uint32_t au_layout_anim_tick(AuLayout *L, uint32_t now_ms, float view_x, float view_y,
                             void (*fn)(void *ctx, AuIRect r), void *ctx)
{
    if (!L)
        return UINT32_MAX;
    L->anim_ms = now_ms;
    for (uint32_t i = 0; i < L->nanims;) {
        LAnim *a = L->anims[i];
        LBlock *k = a->k;
        AuElement *el = k->b.node && au_is_element(k->b.node) ? au_as_element(k->b.node) : NULL;
        /* a box a subtree rebuild dropped, or whose style stopped it */
        if ((k->b.flags & LF_DEAD) || !el || el->box != k || !au_css_animation_playing(k->b.style)) {
            if (a->have_prev)
                emit(a->prev, fn, ctx);
            anim_drop(L, i);
            continue;
        }
        if (!L->env.anim_sample(L->env.ctx, k->b.style, now_ms, &a->s))
            memset(&a->s, 0, sizeof a->s);
        AuRect r;
        if (au_layout_element_visual_rect(L, el, view_x, view_y, &r)) {
            /* the box's overflow (an SVG's strokes, shadows) reaches past its border box */
            float pad = l_max(l_max(-k->ov.x, -k->ov.y), l_max(k->ov.x + k->ov.w - k->w, k->ov.y + k->ov.h - k->h));
            if (pad > 0) {
                r.x -= pad;
                r.y -= pad;
                r.w += 2 * pad;
                r.h += 2 * pad;
            }
            if (a->have_prev)
                emit(a->prev, fn, ctx);
            emit(r, fn, ctx);
            a->prev = r;
            a->have_prev = true;
        }
        i++;
    }
    return L->nanims ? ANIM_FRAME_MS : UINT32_MAX;
}
