/* Computed style storage: the seven reference-counted groups, AuStyle,
 * the initial style, custom property maps, and the accessors layout uses.
 *
 * Groups are shared copy-on-write: css_mut_group gives a private copy
 * before a write. A group's blob and calc fields are retained by every
 * group that holds them; the offsets of those fields come from the
 * property table (props.def). */
#include "css_int.h"

#include <math.h>

/* ---- Group layout ---- */

static const size_t group_sizes[G_COUNT] = {
    sizeof(AuStyleInh), sizeof(AuStyleInhRare), sizeof(AuStyleBox), sizeof(AuStyleSurround),
    sizeof(AuStyleBackground), sizeof(AuStyleFlex), sizeof(AuStyleRare),
};

size_t css_group_size(int g) { return group_sizes[g]; }

/* Offsets of blob and AuLen fields per group. */
#define MAX_FIELDS 64
static uint16_t blob_off[G_COUNT][MAX_FIELDS], len_off[G_COUNT][MAX_FIELDS];
static uint8_t nblob[G_COUNT], nlen[G_COUNT];

static void add_blob(int g, size_t off) { blob_off[g][nblob[g]++] = (uint16_t)off; }
static void add_len(int g, size_t off) { len_off[g][nlen[g]++] = (uint16_t)off; }

static void build_field_tables(void)
{
    for (int p = 1; p < AU_PROP_LONGHAND_END; p++) {
        const CssPropInfo *pi = &css_props[p];
        switch (pi->type) {
        case T_BLOB:
            add_blob(pi->group, pi->offset);
            break;
        case T_LEN:
            add_len(pi->group, pi->offset);
            break;
        default:
            break;
        }
    }
    add_len(G_INH, offsetof(AuStyleInh, line_height));
    add_blob(G_INHR, offsetof(AuStyleInhRare, cursor_images));
    add_len(G_BOX, offsetof(AuStyleBox, vertical_align_len));
    for (int k = 0; k < 4; k++) {
        add_len(G_SURR, offsetof(AuStyleSurround, radius) + (size_t)k * 2 * sizeof(AuLen));
        add_len(G_SURR, offsetof(AuStyleSurround, radius) + (size_t)(k * 2 + 1) * sizeof(AuLen));
    }
}

void *css_style_group(const AuStyle *s, int g)
{
    switch (g) {
    case G_INH: return s->inh;
    case G_INHR: return s->inhr;
    case G_BOX: return s->box;
    case G_SURR: return s->surr;
    case G_BG: return s->bg;
    case G_FLEX: return s->flex;
    default: return s->rare;
    }
}

static void **group_slot(AuStyle *s, int g)
{
    switch (g) {
    case G_INH: return (void **)&s->inh;
    case G_INHR: return (void **)&s->inhr;
    case G_BOX: return (void **)&s->box;
    case G_SURR: return (void **)&s->surr;
    case G_BG: return (void **)&s->bg;
    case G_FLEX: return (void **)&s->flex;
    default: return (void **)&s->rare;
    }
}

void css_group_retain_fields(int g, void *grp)
{
    char *b = grp;
    for (int i = 0; i < nblob[g]; i++)
        au_css_blob_retain(*(AuCssBlob **)(b + blob_off[g][i]));
    for (int i = 0; i < nlen[g]; i++) {
        AuLen *l = (AuLen *)(b + len_off[g][i]);
        if (l->kind == AU_LEN_CALC)
            au_css_blob_retain(l->u.calc);
    }
    if (g == G_INH)
        css_custom_retain(((AuStyleInh *)grp)->custom);
}

void css_group_release_fields(int g, void *grp)
{
    char *b = grp;
    for (int i = 0; i < nblob[g]; i++)
        au_css_blob_release(*(AuCssBlob **)(b + blob_off[g][i]));
    for (int i = 0; i < nlen[g]; i++) {
        AuLen *l = (AuLen *)(b + len_off[g][i]);
        if (l->kind == AU_LEN_CALC)
            au_css_blob_release(l->u.calc);
    }
    if (g == G_INH)
        css_custom_release(((AuStyleInh *)grp)->custom);
}

static void group_retain(void *grp)
{
    if (grp)
        (*(uint32_t *)grp)++;
}

void css_group_release(int g, void *grp)
{
    if (!grp)
        return;
    uint32_t *refs = grp;
    if (--*refs)
        return;
    css_group_release_fields(g, grp);
    au_free(grp);
}

void *css_group_clone(int g, const void *grp)
{
    void *c = au_alloc(group_sizes[g]);
    memcpy(c, grp, group_sizes[g]);
    *(uint32_t *)c = 1;
    css_group_retain_fields(g, c);
    return c;
}

bool css_group_equal(int g, const void *a, const void *b)
{
    if (a == b)
        return true;
    return memcmp((const char *)a + sizeof(uint32_t), (const char *)b + sizeof(uint32_t),
                  group_sizes[g] - sizeof(uint32_t)) == 0;
}

void *css_mut_group(AuStyle *s, int g)
{
    void **slot = group_slot(s, g);
    uint32_t *refs = *slot;
    if (*refs == 1)
        return *slot;
    void *c = css_group_clone(g, *slot);
    css_group_release(g, *slot);
    *slot = c;
    return c;
}

/* ---- Styles ---- */

AuStyle css_initial_style;

AuStyle *css_style_new(void)
{
    AuStyle *s = au_calloc(sizeof *s);
    s->refs = 1;
    return s;
}

AuStyle *au_style_retain(AuStyle *s)
{
    if (s)
        s->refs++;
    return s;
}

void au_style_release(AuStyle *s)
{
    if (!s || --s->refs)
        return;
    for (int g = 0; g < G_COUNT; g++)
        css_group_release(g, css_style_group(s, g));
    /* pseudo-element styles hang off their element's style and have no
     * pseudos of their own, so this recursion is one level deep */
    AuStyle *p = s->pseudos;
    while (p) {
        AuStyle *n = p->next;
        p->next = NULL;
        au_style_release(p);
        p = n;
    }
    au_free(s);
}

const AuStyle *au_style_pseudo(const AuStyle *s, AuCssPseudo p)
{
    for (const AuStyle *x = s ? s->pseudos : NULL; x; x = x->next)
        if (x->pseudo == p)
            return x;
    return NULL;
}

/* ---- Custom properties ---- */

AuCustomProps *css_custom_retain(AuCustomProps *c)
{
    if (c)
        c->refs++;
    return c;
}

void css_custom_release(AuCustomProps *c)
{
    if (!c || --c->refs)
        return;
    for (uint32_t i = 0; i < c->n; i++)
        css_toklist_release(c->e[i].value);
    au_free(c->e);
    au_free(c);
}

CssTokList *css_custom_get(const AuCustomProps *c, AuAtom name)
{
    if (!c)
        return NULL;
    uint32_t lo = 0, hi = c->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c->e[mid].name == name)
            return c->e[mid].value;
        if (c->e[mid].name < name)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

const char *au_style_custom_property(const AuStyle *s, AuAtom name, size_t *len)
{
    static AuBuf buf;
    CssTokList *l = css_custom_get(s->inh->custom, name);
    if (!l)
        return NULL;
    au_buf_clear(&buf);
    css_tokens_serialize(&l->tk, 0, l->tk.n, &buf);
    if (len)
        *len = buf.len;
    return au_buf_cstr(&buf);
}

/* ---- Lengths ---- */

static double calc_eval(const AuCssValue *v, double basis, int depth)
{
    if (depth > 40)
        return 0;
    switch (v->type) {
    case AU_CSSV_NUMBER:
        return v->u.num;
    case AU_CSSV_PERCENT:
        return basis * v->u.num / 100;
    case AU_CSSV_DIMENSION:
        return v->u.num; /* computed calc trees are in px */
    case AU_CSSV_CALC:
        break;
    default:
        return 0;
    }
    const AuCssValue *it = v->u.items;
    double x[32];
    uint32_t n = v->n > 32 ? 32 : v->n;
    switch (v->unit) {
    case AU_CALC_SUM: {
        double s = 0;
        for (uint32_t i = 0; i < v->n; i++)
            s += calc_eval(&it[i], basis, depth + 1);
        return s;
    }
    case AU_CALC_PRODUCT: {
        double p = 1;
        for (uint32_t i = 0; i < v->n; i++)
            p *= calc_eval(&it[i], basis, depth + 1);
        return p;
    }
    case AU_CALC_NEGATE:
        return -calc_eval(&it[0], basis, depth + 1);
    case AU_CALC_INVERT: {
        double d = calc_eval(&it[0], basis, depth + 1);
        return d != 0 ? 1 / d : 0;
    }
    case AU_CALC_MIN:
    case AU_CALC_MAX: {
        double r = calc_eval(&it[0], basis, depth + 1);
        for (uint32_t i = 1; i < v->n; i++) {
            double d = calc_eval(&it[i], basis, depth + 1);
            if (v->unit == AU_CALC_MIN ? d < r : d > r)
                r = d;
        }
        return r;
    }
    case AU_CALC_CLAMP: {
        double val = calc_eval(&it[1], basis, depth + 1);
        if (!css_is_kw(&it[2], AU_K_none)) {
            double mx = calc_eval(&it[2], basis, depth + 1);
            if (val > mx)
                val = mx;
        }
        if (!css_is_kw(&it[0], AU_K_none)) {
            double mn = calc_eval(&it[0], basis, depth + 1);
            if (val < mn)
                val = mn;
        }
        return val;
    }
    default:
        for (uint32_t i = 0; i < n; i++)
            x[i] = calc_eval(&it[i], basis, depth + 1);
        switch (v->unit) {
        case AU_CALC_ROUND_NEAREST: return x[1] ? floor(x[0] / x[1] + 0.5) * x[1] : 0;
        case AU_CALC_ROUND_UP: return x[1] ? ceil(x[0] / x[1]) * x[1] : 0;
        case AU_CALC_ROUND_DOWN: return x[1] ? floor(x[0] / x[1]) * x[1] : 0;
        case AU_CALC_ROUND_TO_ZERO: return x[1] ? (x[0] / x[1] < 0 ? ceil(x[0] / x[1]) : floor(x[0] / x[1])) * x[1] : 0;
        case AU_CALC_MOD: {
            if (!x[1])
                return 0;
            double r = fmod(x[0], x[1]);
            if (r != 0 && ((r < 0) != (x[1] < 0)))
                r += x[1];
            return r;
        }
        case AU_CALC_REM: return x[1] ? fmod(x[0], x[1]) : 0;
        case AU_CALC_ABS: return fabs(x[0]);
        case AU_CALC_SIGN: return x[0] > 0 ? 1 : x[0] < 0 ? -1 : 0;
        case AU_CALC_HYPOT: {
            double s = 0;
            for (uint32_t i = 0; i < n; i++)
                s += x[i] * x[i];
            return sqrt(s);
        }
        default: return 0;
        }
    }
}

float au_len_resolve(const AuLen *l, float basis)
{
    switch (l->kind) {
    case AU_LEN_PX:
    case AU_LEN_FIT_CONTENT_PX:
        return l->u.px;
    case AU_LEN_PCT:
        return basis * l->u.pct / 100;
    case AU_LEN_CALC: {
        double r = calc_eval(&l->u.calc->value, basis, 0);
        return r != r ? 0 : (float)r;
    }
    default:
        return 0;
    }
}

/* ---- Background layers ---- */

size_t au_style_bg_layer_count(const AuStyle *s)
{
    const AuCssBlob *b = s->bg->background_image;
    if (!b)
        return 1;
    return b->value.type == AU_CSSV_COMMA_LIST ? b->value.n : 1;
}

static const AuCssValue *layer_item(const AuCssBlob *b, size_t i)
{
    if (!b)
        return NULL;
    uint32_t n;
    const AuCssValue *it = css_items(&b->value, &n);
    if (b->value.type != AU_CSSV_COMMA_LIST) {
        n = 1;
        it = &b->value;
    }
    return n ? &it[i % n] : NULL;
}

void au_style_bg_layer(const AuStyle *s, size_t i, AuBgLayer *out)
{
    const AuStyleBackground *bg = s->bg;
    memset(out, 0, sizeof *out);
    out->image = layer_item(bg->background_image, i);
    out->position_x = layer_item(bg->background_position_x, i);
    out->position_y = layer_item(bg->background_position_y, i);
    out->size = layer_item(bg->background_size, i);
    const AuCssValue *r = layer_item(bg->background_repeat, i);
    if (r && r->type == AU_CSSV_LIST && r->n == 2) {
        out->repeat_x = r->u.items[0].kw;
        out->repeat_y = r->u.items[1].kw;
    } else {
        out->repeat_x = out->repeat_y = AU_K_repeat;
    }
    const AuCssValue *a = layer_item(bg->background_attachment, i);
    out->attachment = a ? a->kw : AU_K_scroll;
    const AuCssValue *c = layer_item(bg->background_clip, i);
    out->clip = c ? c->kw : AU_K_border_box;
    const AuCssValue *o = layer_item(bg->background_origin, i);
    out->origin = o ? o->kw : AU_K_padding_box;
}

size_t au_style_mask_layer_count(const AuStyle *s)
{
    const AuCssBlob *b = s->rare->mask_image;
    if (!b)
        return 0;
    uint32_t n;
    const AuCssValue *it = css_items(&b->value, &n);
    if (b->value.type != AU_CSSV_COMMA_LIST) {
        n = 1;
        it = &b->value;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!css_is_kw(&it[i], AU_K_none))
            return n;
    return 0;
}

void au_style_mask_layer(const AuStyle *s, size_t i, AuBgLayer *out)
{
    const AuStyleRare *r = s->rare;
    memset(out, 0, sizeof *out);
    out->image = layer_item(r->mask_image, i);
    out->position_x = layer_item(r->mask_position_x, i);
    out->position_y = layer_item(r->mask_position_y, i);
    out->size = layer_item(r->mask_size, i);
    const AuCssValue *rp = layer_item(r->mask_repeat, i);
    if (rp && rp->type == AU_CSSV_LIST && rp->n == 2) {
        out->repeat_x = rp->u.items[0].kw;
        out->repeat_y = rp->u.items[1].kw;
    } else {
        out->repeat_x = out->repeat_y = AU_K_repeat;
    }
    const AuCssValue *m = layer_item(r->mask_mode, i);
    out->attachment = m ? m->kw : AU_K_match_source;
    const AuCssValue *c = layer_item(r->mask_clip, i);
    out->clip = c ? c->kw : AU_K_border_box;
    const AuCssValue *o = layer_item(r->mask_origin, i);
    out->origin = o ? o->kw : AU_K_border_box;
}

/* ---- Initial style ---- */

void css_style_init_defaults(void)
{
    build_field_tables();
    AuStyle *s = &css_initial_style;
    memset(s, 0, sizeof *s);
    s->refs = 1;
    for (int g = 0; g < G_COUNT; g++) {
        void *grp = au_calloc(group_sizes[g]);
        *(uint32_t *)grp = 1;
        *group_slot(s, g) = grp;
    }
    s->inh->font_size = s->inh->font_size_spec = 16;
    s->inh->font_size_kw = 4; /* medium */
    CssComputeCtx c;
    memset(&c, 0, sizeof c);
    AuCssDevice dev;
    au_css_device_default(&dev);
    c.style = s;
    c.parent = NULL;
    c.dev = &dev;
    c.is_root = true;
    c.ex = c.ch = -1;
    c.font_size_for_em = 16;
    for (int p = 1; p < AU_PROP_LONGHAND_END; p++) {
        const AuCssValue *v = css_initial_value((uint16_t)p);
        if (!css_compute_apply(&c, (uint16_t)p, v, NULL)) {
            /* the table's initial value must compute; this is a bug */
        }
    }
    s->box->display_specified = s->box->display;
}

/* Bumps the reference counts of a group shared by the initial style. */
void css_group_share(void *grp);
void css_group_share(void *grp) { group_retain(grp); }
