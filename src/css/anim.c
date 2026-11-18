/* Playing CSS animations (see au_css_animation_sample in css.h). */
#include "css_int.h"

#include <math.h>
#include <string.h>

static const AuCssValue *first_item(const AuCssBlob *b)
{
    if (!b)
        return NULL;
    const AuCssValue *v = &b->value;
    if (v->type == AU_CSSV_COMMA_LIST)
        return v->n ? &v->u.items[0] : NULL;
    return v;
}

static float time_ms(const AuCssBlob *b)
{
    const AuCssValue *v = first_item(b);
    if (!v || v->type != AU_CSSV_DIMENSION)
        return 0;
    return v->unit == AU_UNIT_S ? v->u.num * 1000 : v->unit == AU_UNIT_MS ? v->u.num : 0;
}

static AuAtom name_atom(const AuCssValue *v)
{
    if (!v)
        return 0;
    if (v->type == AU_CSSV_IDENT || v->type == AU_CSSV_STRING)
        return au_atom_lookup(v->u.str, strlen(v->u.str));
    if (v->type == AU_CSSV_KEYWORD && v->kw != AU_K_none) {
        const char *s = au_css_keyword_name((AuCssKeyword)v->kw);
        return s ? au_atom_lookup(s, strlen(s)) : 0;
    }
    return 0;
}

bool au_css_animation_playing(const AuStyle *s)
{
    const AuStyleRare *r = s ? s->rare : NULL;
    if (!r || !r->animation_name || !r->animation_iteration_count)
        return false;
    const AuCssValue *it = first_item(r->animation_iteration_count);
    if (!it || it->type != AU_CSSV_KEYWORD || it->kw != AU_K_infinite)
        return false;
    const AuCssValue *ps = first_item(r->animation_play_state);
    if (ps && ps->type == AU_CSSV_KEYWORD && ps->kw == AU_K_paused)
        return false;
    return name_atom(first_item(r->animation_name)) != 0 && time_ms(r->animation_duration) > 0;
}

/* The y of a cubic Bezier easing curve (0,0) (x1,y1) (x2,y2) (1,1) at x. */
static float bezier(float x1, float y1, float x2, float y2, float x)
{
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 20; i++) {
        t = (lo + hi) / 2;
        float u = 1 - t;
        float bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
        if (bx < x)
            lo = t;
        else
            hi = t;
    }
    float u = 1 - t;
    return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}

static float ease(const AuCssValue *f, float p)
{
    if (!f)
        return bezier(0.25f, 0.1f, 0.25f, 1, p);
    if (f->type == AU_CSSV_KEYWORD) {
        switch (f->kw) {
        case AU_K_linear: return p;
        case AU_K_ease_in: return bezier(0.42f, 0, 1, 1, p);
        case AU_K_ease_out: return bezier(0, 0, 0.58f, 1, p);
        case AU_K_ease_in_out: return bezier(0.42f, 0, 0.58f, 1, p);
        case AU_K_step_start: return p > 0 ? 1 : 0;
        case AU_K_step_end: return p >= 1 ? 1 : 0;
        default: return bezier(0.25f, 0.1f, 0.25f, 1, p);
        }
    }
    if (f->type == AU_CSSV_FUNCTION && f->kw == AU_K_cubic_bezier && f->n == 4)
        return bezier(f->u.items[0].u.num, f->u.items[1].u.num, f->u.items[2].u.num, f->u.items[3].u.num, p);
    if (f->type == AU_CSSV_FUNCTION && f->kw == AU_K_steps && f->n >= 1 && f->u.items[0].u.num >= 1) {
        float n = floorf(f->u.items[0].u.num);
        bool start = f->n > 1 && f->u.items[1].type == AU_CSSV_KEYWORD &&
                     (f->u.items[1].kw == AU_K_jump_start || f->u.items[1].kw == AU_K_start);
        float s = start ? ceilf(p * n) : floorf(p * n);
        return s / n > 1 ? 1 : s / n;
    }
    return p;
}

typedef struct Stop {
    float off;
    const AuCssDecl *d;
} Stop;

/* The keyframes that declare prop, by offset (at most 32). */
static uint32_t stops_for(const AuCssKeyframes *k, uint16_t prop, Stop *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->nframes; i++) {
        const AuCssKeyframe *f = &k->frames[i];
        const AuCssDecl *d = NULL;
        for (uint32_t j = 0; f->decls && j < f->decls->n; j++)
            if (f->decls->d[j].prop == prop && f->decls->d[j].kind != DK_VAR && f->decls->d[j].kind != DK_WIDE)
                d = &f->decls->d[j];
        if (!d)
            continue;
        for (uint32_t o = 0; o < f->noffsets && n < 32; o++) {
            uint32_t at = n++;
            while (at > 0 && out[at - 1].off > f->offsets[o]) {
                out[at] = out[at - 1];
                at--;
            }
            out[at].off = f->offsets[o];
            out[at].d = d;
        }
    }
    return n;
}

/* The two stops around p (a NULL decl is the element's own value at 0 or
 * 1) and the fraction between them. */
static void segment(const Stop *st, uint32_t n, float p, const AuCssDecl **a, const AuCssDecl **b, float *f)
{
    float oa = 0, ob = 1;
    *a = *b = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (st[i].off <= p) {
            oa = st[i].off;
            *a = st[i].d;
        }
    }
    for (uint32_t i = n; i-- > 0;) {
        if (st[i].off >= p) {
            ob = st[i].off;
            *b = st[i].d;
        }
    }
    if (*a == *b || ob <= oa)
        *f = 0;
    else
        *f = (p - oa) / (ob - oa);
}

static float opacity_of(const AuCssDecl *d, float own)
{
    if (!d)
        return own;
    const AuCssValue *v = d->kind == DK_BLOB ? &d->u.blob->value : &d->u.v;
    float x = v->type == AU_CSSV_PERCENT ? v->u.num / 100 : v->type == AU_CSSV_NUMBER ? v->u.num : own;
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

static const AuCssValue *transform_of(const AuCssDecl *d, const AuStyle *s)
{
    if (!d)
        return s->rare->transform ? &s->rare->transform->value : NULL;
    const AuCssValue *v = d->kind == DK_BLOB ? &d->u.blob->value : &d->u.v;
    if (v->type == AU_CSSV_KEYWORD) /* none */
        return NULL;
    return v;
}

bool au_css_animation_sample(AuCssResolver *r, const AuStyle *s, uint32_t now_ms, AuCssAnimSample *out)
{
    memset(out, 0, sizeof *out);
    if (!r || !au_css_animation_playing(s))
        return false;
    const AuStyleRare *rare = s->rare;
    const AuCssKeyframes *k = au_css_resolver_keyframes(r, name_atom(first_item(rare->animation_name)));
    if (!k)
        return false;
    float dur = time_ms(rare->animation_duration);
    float t = (float)now_ms - time_ms(rare->animation_delay);
    if (t < 0)
        return false;
    float cycles = t / dur;
    float whole = floorf(cycles);
    float p = cycles - whole;
    const AuCssValue *dir = first_item(rare->animation_direction);
    int kw = dir && dir->type == AU_CSSV_KEYWORD ? dir->kw : 0;
    bool odd = fmodf(whole, 2) >= 1;
    if (kw == AU_K_reverse || (kw == AU_K_alternate && odd) || (kw == AU_K_alternate_reverse && !odd))
        p = 1 - p;
    Stop st[32];
    const AuCssDecl *a, *b;
    float f;
    const AuCssValue *tf = first_item(rare->animation_timing_function);
    uint32_t n = stops_for(k, AU_PROP_transform, st);
    if (n) {
        segment(st, n, p, &a, &b, &f);
        out->transform = true;
        out->from = transform_of(a, s);
        out->to = transform_of(b, s);
        out->p = ease(tf, f);
    }
    n = stops_for(k, AU_PROP_opacity, st);
    if (n) {
        segment(st, n, p, &a, &b, &f);
        float x = opacity_of(a, rare->opacity), y = opacity_of(b, rare->opacity);
        out->opacity_set = true;
        out->opacity = x + (y - x) * ease(tf, f);
    }
    return out->transform || out->opacity_set;
}
