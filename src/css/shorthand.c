/* Declaration parsing: CSS-wide keywords, values with var(), longhands,
 * and the expansion of every shorthand into its longhands. A shorthand
 * sets all its longhands; the ones not written get their initial value. */
#include "css_int.h"

static CssArena parse_arena;

/* ---- Pushing declarations ---- */

static bool is_scalar(const AuCssValue *v)
{
    switch (v->type) {
    case AU_CSSV_KEYWORD:
    case AU_CSSV_NUMBER:
    case AU_CSSV_PERCENT:
    case AU_CSSV_DIMENSION:
    case AU_CSSV_COLOR:
        return true;
    default:
        return false;
    }
}

static bool has_url(const AuCssValue *v)
{
    if (v->type == AU_CSSV_URL)
        return true;
    if (v->type == AU_CSSV_FUNCTION || v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST)
        for (uint32_t i = 0; i < v->n; i++)
            if (has_url(&v->u.items[i]))
                return true;
    return false;
}

static bool has_attr(const AuCssValue *v)
{
    if (v->type == AU_CSSV_FUNCTION && v->kw == AU_K_attr)
        return true;
    if (v->type == AU_CSSV_FUNCTION || v->type == AU_CSSV_LIST || v->type == AU_CSSV_COMMA_LIST)
        for (uint32_t i = 0; i < v->n; i++)
            if (has_attr(&v->u.items[i]))
                return true;
    return false;
}

static AuCssDecl *new_decl(AuCssDeclBlock *out, uint16_t prop, uint8_t flags, uint8_t kind)
{
    AuCssDecl *d = CSS_PUSH(out->d, out->n, out->cap);
    memset(d, 0, sizeof *d);
    d->prop = prop;
    d->flags = flags;
    d->kind = kind;
    return d;
}

void css_push_decl(AuCssDeclBlock *out, uint16_t prop, const AuCssValue *v, uint8_t flags, const char *base_url)
{
    uint8_t f = (uint8_t)(flags & DF_IMPORTANT);
    if (css_value_needs_compute(v))
        f |= DF_NEEDS_COMPUTE;
    if (has_attr(v))
        f |= DF_HAS_ATTR | DF_NEEDS_COMPUTE;
    if (is_scalar(v)) {
        AuCssDecl *d = new_decl(out, prop, f, DK_VALUE);
        d->u.v = *v;
        return;
    }
    AuCssDecl *d = new_decl(out, prop, f, DK_BLOB);
    d->u.blob = has_url(v) ? css_blob_pack(v, base_url) : css_blob_pack_shared(v);
}

static void push_wide(AuCssDeclBlock *out, uint16_t prop, uint16_t kw, uint8_t flags)
{
    AuCssDecl *d = new_decl(out, prop, (uint8_t)(flags & DF_IMPORTANT), DK_WIDE);
    d->u.v.type = AU_CSSV_KEYWORD;
    d->u.v.kw = kw;
}

void css_decl_retain(AuCssDecl *d)
{
    if (d->kind == DK_BLOB)
        au_css_blob_retain(d->u.blob);
    else if (d->kind == DK_VAR || d->kind == DK_CUSTOM)
        css_toklist_retain(d->u.toks);
}

void css_decl_release(AuCssDecl *d)
{
    if (d->kind == DK_BLOB)
        au_css_blob_release(d->u.blob);
    else if (d->kind == DK_VAR || d->kind == DK_CUSTOM)
        css_toklist_release(d->u.toks);
}

void css_decl_block_init(AuCssDeclBlock *b) { memset(b, 0, sizeof *b); }

void css_decl_block_clear(AuCssDeclBlock *b)
{
    for (uint32_t i = 0; i < b->n; i++)
        css_decl_release(&b->d[i]);
    au_free(b->d);
    memset(b, 0, sizeof *b);
}

static void truncate_block(AuCssDeclBlock *b, uint32_t n)
{
    while (b->n > n)
        css_decl_release(&b->d[--b->n]);
}

/* ---- Initial values ---- */

static AuCssBlob *initial_blobs[AU_PROP_LONGHAND_END];

const AuCssValue *css_initial_value(uint16_t prop)
{
    if (prop >= AU_PROP_LONGHAND_END)
        prop = css_logical_template(prop);
    if (!initial_blobs[prop]) {
        /* kept for good, for every document: no scope, no set */
        AuAtomCollect ac = au_atom_collect_begin(NULL, 0);
        CssTokens tk = {0};
        const char *s = css_props[prop].initial;
        css_tokenize(s, strlen(s), &tk);
        CssArena a = {0};
        CssParseCtx cx = {&a, false, false, NULL};
        AuCssValue *v = css_parse_value(&cx, &tk, 0, tk.n);
        AuCssValue *ok = v ? css_validate_longhand(&cx, prop, v) : NULL;
        if (!ok) {
            /* a table error; fall back to the keyword initial */
            ok = css_new_kw(&a, AU_K_initial);
        }
        initial_blobs[prop] = css_blob_pack(ok, NULL);
        css_arena_free(&a);
        css_tokens_free(&tk);
        au_atom_collect_end(ac);
    }
    return &initial_blobs[prop]->value;
}

static void push_initial(AuCssDeclBlock *out, uint16_t prop, uint8_t flags)
{
    css_push_decl(out, prop, css_initial_value(prop), flags, NULL);
}

/* ---- Entry points ---- */

static bool is_wide(uint16_t kw)
{
    return kw == AU_K_initial || kw == AU_K_inherit || kw == AU_K_unset || kw == AU_K_revert ||
           kw == AU_K_revert_layer;
}

static uint16_t validate_prop(uint16_t prop) { return prop < AU_PROP_LONGHAND_END ? prop : css_logical_template(prop); }

bool css_parse_property(uint16_t prop, const CssTokens *tk, uint32_t a, uint32_t b, bool important,
                        AuCssDeclBlock *out, bool quirks, const char *base_url)
{
    while (a < b && tk->t[a].type == TK_WS)
        a++;
    while (b > a && tk->t[b - 1].type == TK_WS)
        b--;
    if (a >= b || !prop || prop >= AU_PROP_COUNT)
        return false;
    uint8_t flags = important ? DF_IMPORTANT : 0;
    bool sh = prop >= AU_PROP_LOGICAL_END;
    if (b - a == 1 && tk->t[a].type == TK_IDENT && is_wide(tk->t[a].kw)) {
        if (sh) {
            const uint16_t *lh = css_shorthand_longhands(prop);
            for (; *lh; lh++)
                push_wide(out, *lh, tk->t[a].kw, flags);
        } else {
            push_wide(out, prop, tk->t[a].kw, flags);
        }
        return true;
    }
    if (css_tokens_have_var(tk, a, b)) {
        CssTokList *l = css_toklist_new(tk, a, b);
        if (sh) {
            if (prop == AU_PROP_all) {
                css_toklist_release(l);
                return false;
            }
            const uint16_t *lh = css_shorthand_longhands(prop);
            for (; *lh; lh++) {
                AuCssDecl *d = new_decl(out, *lh, flags, DK_VAR);
                d->aux = prop;
                d->u.toks = css_toklist_retain(l);
            }
        } else {
            AuCssDecl *d = new_decl(out, prop, flags, DK_VAR);
            d->u.toks = css_toklist_retain(l);
        }
        css_toklist_release(l);
        return true;
    }
    css_arena_reset(&parse_arena);
    CssParseCtx cx = {&parse_arena, quirks, prop == AU_PROP_content, base_url};
    if (sh) {
        uint32_t n0 = out->n;
        if (css_parse_shorthand(&cx, prop, tk, a, b, out, flags))
            return true;
        truncate_block(out, n0);
        return false;
    }
    AuCssValue *v = css_parse_value(&cx, tk, a, b);
    v = css_validate_longhand(&cx, validate_prop(prop), v);
    if (!v)
        return false;
    css_push_decl(out, prop, v, flags, base_url);
    return true;
}

/* ---- Shorthand helpers ---- */

/* A copy of a component, so validation can rewrite it in place. */
static AuCssValue *dup(CssParseCtx *cx, const AuCssValue *v) { return css_value_copy(cx->arena, v); }

/* Validates a copy of v as prop; returns the stored tree or NULL. */
static AuCssValue *try_lh(CssParseCtx *cx, uint16_t prop, const AuCssValue *v)
{
    return css_validate_longhand(cx, validate_prop(prop), dup(cx, v));
}

static AuCssValue *list_of(CssParseCtx *cx, AuCssValue **items, uint32_t n, int type)
{
    if (n == 1 && type == AU_CSSV_LIST)
        return items[0];
    AuCssValue *l = css_new_list(cx->arena, type, n);
    for (uint32_t i = 0; i < n; i++)
        l->u.items[i] = *items[i];
    return l;
}

typedef struct {
    CssParseCtx *cx;
    AuCssDeclBlock *out;
    uint8_t flags;
} Sh;

static void emit(Sh *s, uint16_t prop, const AuCssValue *v)
{
    if (v)
        css_push_decl(s->out, prop, v, s->flags, s->cx->base_url);
    else
        push_initial(s->out, prop, s->flags);
}

/* Components of the whole value; fails on commas unless allowed. */
static const AuCssValue *sh_items(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b, uint32_t *n,
                                  bool commas, AuCssValue **whole)
{
    AuCssValue *v = css_parse_value(cx, tk, a, b);
    if (whole)
        *whole = v;
    if (!v || (!commas && v->type == AU_CSSV_COMMA_LIST))
        return NULL;
    return css_items(v, n);
}

/* ---- Box sides ---- */

static bool sh_sides(Sh *s, const uint16_t *lh, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 4)
        return false;
    AuCssValue *v[4];
    for (uint32_t i = 0; i < n; i++)
        if (!(v[i] = try_lh(s->cx, lh[0], &it[i])))
            return false;
    AuCssValue *t = v[0], *r = n > 1 ? v[1] : t, *bo = n > 2 ? v[2] : t, *l = n > 3 ? v[3] : r;
    emit(s, lh[0], t);
    emit(s, lh[1], r);
    emit(s, lh[2], bo);
    emit(s, lh[3], l);
    return true;
}

/* width || style || color, for border shorthands. */
static bool border_parts(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b, AuCssValue **w, AuCssValue **st,
                         AuCssValue **c)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    *w = *st = *c = NULL;
    if (!it || n < 1 || n > 3)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue *x;
        if (!*w && (x = try_lh(s->cx, AU_PROP_border_top_width, &it[i])))
            *w = x;
        else if (!*st && (x = try_lh(s->cx, AU_PROP_border_top_style, &it[i])))
            *st = x;
        else if (!*c && (x = try_lh(s->cx, AU_PROP_border_top_color, &it[i])))
            *c = x;
        else
            return false;
    }
    return true;
}

static bool sh_border_list(Sh *s, const uint16_t *lh, const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuCssValue *w, *st, *c;
    if (!border_parts(s, tk, a, b, &w, &st, &c))
        return false;
    for (; *lh; lh += 3) {
        emit(s, lh[0], w);
        emit(s, lh[1], st);
        emit(s, lh[2], c);
    }
    return true;
}

static bool sh_border_radius(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it)
        return false;
    AuCssValue *h[4], *v[4];
    uint32_t nh = 0, nv = 0;
    bool slash = false;
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].type == AU_CSSV_DELIM && it[i].unit == '/') {
            if (slash || nh == 0)
                return false;
            slash = true;
            continue;
        }
        AuCssValue *x = dup(s->cx, &it[i]);
        if (!css_validate_longhand(s->cx, AU_PROP_padding_top, x) || x->type == AU_CSSV_KEYWORD)
            return false;
        if (slash) {
            if (nv >= 4)
                return false;
            v[nv++] = x;
        } else {
            if (nh >= 4)
                return false;
            h[nh++] = x;
        }
    }
    if (nh == 0 || (slash && nv == 0))
        return false;
    if (!slash) {
        for (uint32_t i = 0; i < nh; i++)
            v[i] = h[i];
        nv = nh;
    }
    AuCssValue *H[4] = {h[0], nh > 1 ? h[1] : h[0], nh > 2 ? h[2] : h[0], nh > 3 ? h[3] : (nh > 1 ? h[1] : h[0])};
    AuCssValue *V[4] = {v[0], nv > 1 ? v[1] : v[0], nv > 2 ? v[2] : v[0], nv > 3 ? v[3] : (nv > 1 ? v[1] : v[0])};
    static const uint16_t corners[4] = {AU_PROP_border_top_left_radius, AU_PROP_border_top_right_radius,
                                        AU_PROP_border_bottom_right_radius, AU_PROP_border_bottom_left_radius};
    for (int k = 0; k < 4; k++) {
        AuCssValue *pair[2] = {H[k], V[k]};
        /* one value when both radii are the same */
        emit(s, corners[k], css_value_equal(H[k], V[k]) ? H[k] : list_of(s->cx, pair, 2, AU_CSSV_LIST));
    }
    return true;
}

static bool sh_outline(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n > 3)
        return false;
    AuCssValue *w = NULL, *st = NULL, *c = NULL, *x;
    for (uint32_t i = 0; i < n; i++) {
        if (!w && (x = try_lh(s->cx, AU_PROP_outline_width, &it[i])))
            w = x;
        else if (!st && (x = try_lh(s->cx, AU_PROP_outline_style, &it[i])))
            st = x;
        else if (!c && (x = try_lh(s->cx, AU_PROP_outline_color, &it[i])))
            c = x;
        else
            return false;
    }
    emit(s, AU_PROP_outline_color, c);
    emit(s, AU_PROP_outline_style, st);
    emit(s, AU_PROP_outline_width, w);
    return true;
}

/* ---- Background ---- */

AuCssValue *css_parse_position(CssParseCtx *cx, AuCssValue *v);

static bool is_pos_part(CssParseCtx *cx, const AuCssValue *v)
{
    if (css_is_kw(v, AU_K_left) || css_is_kw(v, AU_K_right) || css_is_kw(v, AU_K_top) || css_is_kw(v, AU_K_bottom) ||
        css_is_kw(v, AU_K_center))
        return true;
    AuCssValue *c = dup(cx, v);
    return css_validate_longhand(cx, AU_PROP_left, c) && c->type != AU_CSSV_KEYWORD;
}

typedef struct {
    AuCssValue *image, *px, *py, *size, *repeat, *attach, *origin, *clip, *color;
} BgLayer;

/* The longhands a layer of the background or mask shorthand sets: image,
 * size, repeat, attachment (mask-mode for masks), clip, colour (0 for
 * masks). */
enum { LI_IMAGE, LI_SIZE, LI_REPEAT, LI_ATTACH, LI_CLIP, LI_COLOR };
static const uint16_t bg_ids[6] = {AU_PROP_background_image, AU_PROP_background_size, AU_PROP_background_repeat,
                                   AU_PROP_background_attachment, AU_PROP_background_clip,
                                   AU_PROP_background_color};
static const uint16_t mask_ids[6] = {AU_PROP_mask_image, AU_PROP_mask_size, AU_PROP_mask_repeat, AU_PROP_mask_mode,
                                     AU_PROP_mask_clip, 0};

static bool bg_layer(CssParseCtx *cx, const AuCssValue *item, BgLayer *L, bool last, const uint16_t *ids)
{
    uint32_t n;
    const AuCssValue *it = css_items(item, &n);
    memset(L, 0, sizeof *L);
    uint32_t i = 0;
    while (i < n) {
        AuCssValue *x;
        if (!L->px && is_pos_part(cx, &it[i])) {
            uint32_t j = i;
            while (j < n && j - i < 4 && is_pos_part(cx, &it[j]))
                j++;
            /* the longest prefix that is a valid position */
            uint32_t k;
            AuCssValue *pos = NULL;
            for (k = j; k > i; k--) {
                AuCssValue *seq[4];
                for (uint32_t m = i; m < k; m++)
                    seq[m - i] = dup(cx, &it[m]);
                pos = css_parse_position(cx, list_of(cx, seq, k - i, AU_CSSV_LIST));
                if (pos)
                    break;
            }
            if (!pos)
                return false;
            L->px = &pos->u.items[0];
            L->py = &pos->u.items[1];
            i = k;
            if (i < n && it[i].type == AU_CSSV_DELIM && it[i].unit == '/') {
                i++;
                AuCssValue *sz[2];
                uint32_t ns = 0;
                while (i < n && ns < 2) {
                    AuCssValue *c = dup(cx, &it[i]);
                    if (css_is_kw(c, AU_K_cover) || css_is_kw(c, AU_K_contain)) {
                        if (ns)
                            break;
                        sz[ns++] = c;
                        i++;
                        break;
                    }
                    if (!css_is_kw(c, AU_K_auto_) &&
                        !(css_validate_longhand(cx, AU_PROP_width, c) && c->type != AU_CSSV_KEYWORD))
                        break;
                    sz[ns++] = c;
                    i++;
                }
                if (!ns)
                    return false;
                x = css_validate_longhand(cx, ids[LI_SIZE], list_of(cx, sz, ns, AU_CSSV_LIST));
                if (!x)
                    return false;
                L->size = &x->u.items[0];
            }
            continue;
        }
        if (!L->image && (x = try_lh(cx, ids[LI_IMAGE], &it[i]))) {
            L->image = &x->u.items[0];
            i++;
            continue;
        }
        if (!L->repeat) {
            /* one or two repeat keywords */
            AuCssValue *seq[2];
            uint32_t m = 0;
            while (m < 2 && i + m < n &&
                   (css_is_kw(&it[i + m], AU_K_repeat) || css_is_kw(&it[i + m], AU_K_space) ||
                    css_is_kw(&it[i + m], AU_K_round) || css_is_kw(&it[i + m], AU_K_no_repeat) ||
                    ((css_is_kw(&it[i + m], AU_K_repeat_x) || css_is_kw(&it[i + m], AU_K_repeat_y)) && m == 0))) {
                seq[m] = dup(cx, &it[i + m]);
                m++;
                if (css_is_kw(seq[0], AU_K_repeat_x) || css_is_kw(seq[0], AU_K_repeat_y))
                    break;
            }
            if (m) {
                x = css_validate_longhand(cx, ids[LI_REPEAT], list_of(cx, seq, m, AU_CSSV_LIST));
                if (!x)
                    return false;
                L->repeat = &x->u.items[0];
                i += m;
                continue;
            }
        }
        if (!L->attach && (x = try_lh(cx, ids[LI_ATTACH], &it[i]))) {
            L->attach = &x->u.items[0];
            i++;
            continue;
        }
        if ((!L->origin || !L->clip) && (x = try_lh(cx, ids[LI_CLIP], &it[i]))) {
            if (!L->origin) {
                if (css_is_kw(&x->u.items[0], AU_K_text) || css_is_kw(&x->u.items[0], AU_K_no_clip))
                    L->clip = &x->u.items[0];
                else
                    L->origin = &x->u.items[0];
            } else {
                L->clip = &x->u.items[0];
            }
            i++;
            continue;
        }
        if (last && ids[LI_COLOR] && !L->color && (x = try_lh(cx, ids[LI_COLOR], &it[i]))) {
            L->color = x;
            i++;
            continue;
        }
        return false;
    }
    if (L->origin && !L->clip)
        L->clip = L->origin;
    return true;
}

/* background, or (mask) the mask shorthand: the same layer grammar
 * without attachment and colour, with mask-mode. */
static bool sh_background(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b, bool mask)
{
    CssParseCtx *cx = s->cx;
    AuCssValue *whole = css_parse_value(cx, tk, a, b);
    if (!whole)
        return false;
    uint32_t nl;
    const AuCssValue *layers = css_items(whole, &nl);
    if (whole->type != AU_CSSV_COMMA_LIST) {
        nl = 1;
        layers = whole;
    }
    BgLayer *L = css_arena_alloc(cx->arena, nl * sizeof(BgLayer));
    for (uint32_t i = 0; i < nl; i++)
        if (!bg_layer(cx, &layers[i], &L[i], i == nl - 1, mask ? mask_ids : bg_ids))
            return false;
    static const uint16_t bprops[8] = {AU_PROP_background_image, AU_PROP_background_position_x,
                                       AU_PROP_background_position_y, AU_PROP_background_size,
                                       AU_PROP_background_repeat, AU_PROP_background_attachment,
                                       AU_PROP_background_origin, AU_PROP_background_clip};
    static const uint16_t mprops[8] = {AU_PROP_mask_image, AU_PROP_mask_position_x, AU_PROP_mask_position_y,
                                       AU_PROP_mask_size, AU_PROP_mask_repeat, AU_PROP_mask_mode,
                                       AU_PROP_mask_origin, AU_PROP_mask_clip};
    const uint16_t *props = mask ? mprops : bprops;
    for (int p = 0; p < 8; p++) {
        AuCssValue *l = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, nl);
        const AuCssValue *init = css_initial_value(props[p]);
        uint32_t ni;
        const AuCssValue *initem = css_items(init, &ni);
        for (uint32_t i = 0; i < nl; i++) {
            AuCssValue *v = NULL;
            switch (p) {
            case 0: v = L[i].image; break;
            case 1: v = L[i].px; break;
            case 2: v = L[i].py; break;
            case 3: v = L[i].size; break;
            case 4: v = L[i].repeat; break;
            case 5: v = L[i].attach; break;
            case 6: v = L[i].origin; break;
            case 7: v = L[i].clip; break;
            }
            l->u.items[i] = v ? *v : initem[0];
        }
        emit(s, props[p], l);
    }
    if (!mask)
        emit(s, AU_PROP_background_color, L[nl - 1].color);
    return true;
}

static bool sh_background_position(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b, bool mask)
{
    CssParseCtx *cx = s->cx;
    AuCssValue *whole = css_parse_value(cx, tk, a, b);
    if (!whole)
        return false;
    AuCssValue *list = whole->type == AU_CSSV_COMMA_LIST ? whole : NULL;
    uint32_t n = list ? list->n : 1;
    AuCssValue *xs = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, n);
    AuCssValue *ys = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, n);
    for (uint32_t i = 0; i < n; i++) {
        AuCssValue *pos = css_parse_position(cx, list ? &list->u.items[i] : whole);
        if (!pos)
            return false;
        xs->u.items[i] = pos->u.items[0];
        ys->u.items[i] = pos->u.items[1];
    }
    emit(s, mask ? AU_PROP_mask_position_x : AU_PROP_background_position_x, xs);
    emit(s, mask ? AU_PROP_mask_position_y : AU_PROP_background_position_y, ys);
    return true;
}

/* ---- Fonts ---- */

static bool sh_font(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    CssParseCtx *cx = s->cx;
    AuCssValue *whole = css_parse_value(cx, tk, a, b);
    if (!whole)
        return false;
    /* system fonts */
    if (whole->type == AU_CSSV_KEYWORD &&
        (whole->kw == AU_K_caption || whole->kw == AU_K_icon || whole->kw == AU_K_menu ||
         whole->kw == AU_K_message_box || whole->kw == AU_K_small_caption || whole->kw == AU_K_status_bar)) {
        emit(s, AU_PROP_font_style, NULL);
        emit(s, AU_PROP_font_variant_caps, NULL);
        emit(s, AU_PROP_font_weight, NULL);
        emit(s, AU_PROP_font_stretch, NULL);
        emit(s, AU_PROP_font_size, css_new_num(cx->arena, AU_CSSV_DIMENSION, 13, AU_UNIT_PX));
        emit(s, AU_PROP_line_height, NULL);
        AuCssValue *fam = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, 1);
        fam->u.items[0] = *css_new_kw(cx->arena, AU_K_system_ui);
        emit(s, AU_PROP_font_family, fam);
        emit(s, AU_PROP_font_kerning, NULL);
        return true;
    }
    uint32_t nfirst;
    const AuCssValue *first = css_items(whole->type == AU_CSSV_COMMA_LIST ? &whole->u.items[0] : whole, &nfirst);
    AuCssValue *style = NULL, *variant = NULL, *weight = NULL, *stretch = NULL, *size = NULL, *lh = NULL, *x;
    uint32_t i = 0, nnormal = 0;
    for (; i < nfirst; i++) {
        const AuCssValue *c = &first[i];
        if (css_is_kw(c, AU_K_normal)) {
            nnormal++;
            continue;
        }
        if (!style && (css_is_kw(c, AU_K_italic) || css_is_kw(c, AU_K_oblique))) {
            style = dup(cx, c);
            continue;
        }
        if (!variant && css_is_kw(c, AU_K_small_caps)) {
            variant = dup(cx, c);
            continue;
        }
        if (!weight && c->type != AU_CSSV_DIMENSION && c->type != AU_CSSV_PERCENT &&
            (x = try_lh(cx, AU_PROP_font_weight, c))) {
            weight = x;
            continue;
        }
        if (!stretch && c->type == AU_CSSV_KEYWORD && (x = try_lh(cx, AU_PROP_font_stretch, c))) {
            stretch = x;
            continue;
        }
        break;
    }
    if ((style != NULL) + (variant != NULL) + (weight != NULL) + (stretch != NULL) + nnormal > 4)
        return false;
    if (i >= nfirst || !(size = try_lh(cx, AU_PROP_font_size, &first[i])))
        return false;
    i++;
    if (i < nfirst && first[i].type == AU_CSSV_DELIM && first[i].unit == '/') {
        i++;
        if (i >= nfirst || !(lh = try_lh(cx, AU_PROP_line_height, &first[i])))
            return false;
        i++;
    }
    if (i >= nfirst)
        return false; /* a family is required */
    /* family: the rest of the first item, then the other comma items */
    uint32_t nrest = whole->type == AU_CSSV_COMMA_LIST ? whole->n : 1;
    AuCssValue *fam = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, nrest);
    AuCssValue *head[64];
    uint32_t nh = 0;
    for (; i < nfirst && nh < 64; i++)
        head[nh++] = dup(cx, &first[i]);
    fam->u.items[0] = *list_of(cx, head, nh, AU_CSSV_LIST);
    for (uint32_t k = 1; k < nrest; k++)
        fam->u.items[k] = *dup(cx, &whole->u.items[k]);
    AuCssValue *f = css_validate_longhand(cx, AU_PROP_font_family, fam);
    if (!f)
        return false;
    emit(s, AU_PROP_font_style, style);
    emit(s, AU_PROP_font_variant_caps, variant);
    emit(s, AU_PROP_font_weight, weight);
    emit(s, AU_PROP_font_stretch, stretch);
    emit(s, AU_PROP_font_size, size);
    emit(s, AU_PROP_line_height, lh);
    emit(s, AU_PROP_font_family, f);
    emit(s, AU_PROP_font_kerning, NULL);
    return true;
}

static bool sh_font_variant(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n != 1)
        return false;
    if (css_is_kw(&it[0], AU_K_none)) {
        emit(s, AU_PROP_font_variant_caps, NULL);
        return true;
    }
    AuCssValue *x = try_lh(s->cx, AU_PROP_font_variant_caps, &it[0]);
    if (!x)
        return false;
    emit(s, AU_PROP_font_variant_caps, x);
    return true;
}

/* ---- Lists, text ---- */

static bool sh_list_style(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n > 3)
        return false;
    AuCssValue *pos = NULL, *img = NULL, *type = NULL, *x;
    int nones = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (css_is_kw(&it[i], AU_K_none)) {
            nones++;
            continue;
        }
        if (!pos && (x = try_lh(s->cx, AU_PROP_list_style_position, &it[i])))
            pos = x;
        else if (!img && (x = try_lh(s->cx, AU_PROP_list_style_image, &it[i])))
            img = x;
        else if (!type && (x = try_lh(s->cx, AU_PROP_list_style_type, &it[i])))
            type = x;
        else
            return false;
    }
    AuCssValue *none = css_new_kw(s->cx->arena, AU_K_none);
    if (nones > 2 || (nones == 2 && (img || type)) || (nones == 1 && img && type))
        return false;
    if (nones == 2) {
        img = none;
        type = none;
    } else if (nones == 1) {
        if (!type)
            type = none;
        else
            img = none;
    }
    emit(s, AU_PROP_list_style_position, pos);
    emit(s, AU_PROP_list_style_image, img);
    emit(s, AU_PROP_list_style_type, type);
    return true;
}

static bool sh_text_decoration(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it)
        return false;
    AuCssValue *lines[4], *style = NULL, *color = NULL, *thick = NULL, *x;
    uint32_t nlines = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (nlines < 4 && (css_is_kw(&it[i], AU_K_underline) || css_is_kw(&it[i], AU_K_overline) ||
                           css_is_kw(&it[i], AU_K_line_through) || css_is_kw(&it[i], AU_K_blink) ||
                           (css_is_kw(&it[i], AU_K_none) && nlines == 0))) {
            lines[nlines++] = dup(s->cx, &it[i]);
            continue;
        }
        if (!style && (x = try_lh(s->cx, AU_PROP_text_decoration_style, &it[i])))
            style = x;
        else if (!thick && (x = try_lh(s->cx, AU_PROP_text_decoration_thickness, &it[i])))
            thick = x;
        else if (!color && (x = try_lh(s->cx, AU_PROP_text_decoration_color, &it[i])))
            color = x;
        else
            return false;
    }
    AuCssValue *line = NULL;
    if (nlines) {
        line = css_validate_longhand(s->cx, AU_PROP_text_decoration_line, list_of(s->cx, lines, nlines, AU_CSSV_LIST));
        if (!line)
            return false;
    }
    emit(s, AU_PROP_text_decoration_line, line);
    emit(s, AU_PROP_text_decoration_thickness, thick);
    emit(s, AU_PROP_text_decoration_style, style);
    emit(s, AU_PROP_text_decoration_color, color);
    return true;
}

static bool sh_white_space(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 2)
        return false;
    CssArena *ar = s->cx->arena;
    uint16_t col = 0, wrap = 0;
    if (n == 1 && it[0].type == AU_CSSV_KEYWORD) {
        switch (it[0].kw) {
        case AU_K_normal: col = AU_K_collapse; wrap = AU_K_wrap; break;
        case AU_K_pre: col = AU_K_preserve; wrap = AU_K_nowrap; break;
        case AU_K_nowrap: col = AU_K_collapse; wrap = AU_K_nowrap; break;
        case AU_K_pre_wrap: col = AU_K_preserve; wrap = AU_K_wrap; break;
        case AU_K_break_spaces: col = AU_K_break_spaces; wrap = AU_K_wrap; break;
        case AU_K_pre_line: col = AU_K_preserve_breaks; wrap = AU_K_wrap; break;
        default: break;
        }
    }
    if (!col) {
        AuCssValue *x;
        for (uint32_t i = 0; i < n; i++) {
            if (!col && (x = try_lh(s->cx, AU_PROP_white_space_collapse, &it[i])))
                col = x->kw;
            else if (!wrap && (x = try_lh(s->cx, AU_PROP_text_wrap_mode, &it[i])))
                wrap = x->kw;
            else
                return false;
        }
        if (!col)
            col = AU_K_collapse;
        if (!wrap)
            wrap = AU_K_wrap;
    }
    emit(s, AU_PROP_white_space_collapse, css_new_kw(ar, col));
    emit(s, AU_PROP_text_wrap_mode, css_new_kw(ar, wrap));
    return true;
}

static bool sh_text_wrap(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n > 2)
        return false;
    AuCssValue *mode = NULL, *style = NULL, *x;
    for (uint32_t i = 0; i < n; i++) {
        if (!mode && (x = try_lh(s->cx, AU_PROP_text_wrap_mode, &it[i])))
            mode = x;
        else if (!style && (x = try_lh(s->cx, AU_PROP_text_wrap_style, &it[i])))
            style = x;
        else
            return false;
    }
    emit(s, AU_PROP_text_wrap_mode, mode);
    emit(s, AU_PROP_text_wrap_style, style);
    return true;
}

static bool sh_overflow(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 2)
        return false;
    AuCssValue *x = try_lh(s->cx, AU_PROP_overflow_x, &it[0]);
    AuCssValue *y = n == 2 ? try_lh(s->cx, AU_PROP_overflow_y, &it[1]) : x;
    if (!x || !y)
        return false;
    emit(s, AU_PROP_overflow_x, x);
    emit(s, AU_PROP_overflow_y, y);
    return true;
}

/* overscroll-behavior: x [y] */
static bool sh_overscroll(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 2)
        return false;
    AuCssValue *x = try_lh(s->cx, AU_PROP_overscroll_behavior_x, &it[0]);
    AuCssValue *y = n == 2 ? try_lh(s->cx, AU_PROP_overscroll_behavior_y, &it[1]) : x;
    if (!x || !y)
        return false;
    emit(s, AU_PROP_overscroll_behavior_x, x);
    emit(s, AU_PROP_overscroll_behavior_y, y);
    return true;
}

/* container: <'container-name'> [ / <'container-type'> ]? */
static bool sh_container(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1)
        return false;
    uint32_t slash = n;
    for (uint32_t i = 0; i < n; i++)
        if (it[i].type == AU_CSSV_DELIM) {
            if (it[i].unit != '/' || slash != n)
                return false;
            slash = i;
        }
    if (slash == 0 || (slash < n && slash + 2 != n))
        return false;
    AuCssValue *names = css_new_list(s->cx->arena, AU_CSSV_LIST, slash);
    for (uint32_t i = 0; i < slash; i++)
        names->u.items[i] = it[i];
    AuCssValue *name = try_lh(s->cx, AU_PROP_container_name, slash == 1 ? &it[0] : names);
    AuCssValue *type = slash < n ? try_lh(s->cx, AU_PROP_container_type, &it[slash + 1]) : NULL;
    if (!name || (slash < n && !type))
        return false;
    emit(s, AU_PROP_container_name, name);
    emit(s, AU_PROP_container_type, type);
    return true;
}

/* ---- Flex, alignment, gap ---- */

static bool sh_flex(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    CssArena *ar = s->cx->arena;
    if (!it || n < 1 || n > 3)
        return false;
    if (n == 1 && css_is_kw(&it[0], AU_K_none)) {
        emit(s, AU_PROP_flex_grow, css_new_num(ar, AU_CSSV_NUMBER, 0, 1));
        emit(s, AU_PROP_flex_shrink, css_new_num(ar, AU_CSSV_NUMBER, 0, 1));
        emit(s, AU_PROP_flex_basis, css_new_kw(ar, AU_K_auto_));
        return true;
    }
    if (n == 1 && css_is_kw(&it[0], AU_K_auto_)) {
        emit(s, AU_PROP_flex_grow, css_new_num(ar, AU_CSSV_NUMBER, 1, 1));
        emit(s, AU_PROP_flex_shrink, css_new_num(ar, AU_CSSV_NUMBER, 1, 1));
        emit(s, AU_PROP_flex_basis, css_new_kw(ar, AU_K_auto_));
        return true;
    }
    AuCssValue *grow = NULL, *shrink = NULL, *basis = NULL, *x;
    for (uint32_t i = 0; i < n; i++) {
        bool num = it[i].type == AU_CSSV_NUMBER || (it[i].type == AU_CSSV_CALC && css_value_type(&it[i]) == CT_NUMBER);
        if (num && !grow) {
            if (!(grow = try_lh(s->cx, AU_PROP_flex_grow, &it[i])))
                return false;
            if (i + 1 < n && (it[i + 1].type == AU_CSSV_NUMBER ||
                              (it[i + 1].type == AU_CSSV_CALC && css_value_type(&it[i + 1]) == CT_NUMBER))) {
                if (!(shrink = try_lh(s->cx, AU_PROP_flex_shrink, &it[i + 1])))
                    return false;
                i++;
            }
            continue;
        }
        if (!basis && (x = try_lh(s->cx, AU_PROP_flex_basis, &it[i]))) {
            if (grow && !shrink && i + 1 < n)
                return false; /* basis must not split grow and shrink */
            basis = x;
            continue;
        }
        return false;
    }
    if (!grow && !basis)
        return false;
    emit(s, AU_PROP_flex_grow, grow ? grow : css_new_num(ar, AU_CSSV_NUMBER, 1, 1));
    emit(s, AU_PROP_flex_shrink, shrink ? shrink : css_new_num(ar, AU_CSSV_NUMBER, 1, 1));
    emit(s, AU_PROP_flex_basis, basis ? basis : css_new_num(ar, AU_CSSV_PERCENT, 0, 0));
    return true;
}

static bool sh_flex_flow(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n > 2)
        return false;
    AuCssValue *dir = NULL, *wrap = NULL, *x;
    for (uint32_t i = 0; i < n; i++) {
        if (!dir && (x = try_lh(s->cx, AU_PROP_flex_direction, &it[i])))
            dir = x;
        else if (!wrap && (x = try_lh(s->cx, AU_PROP_flex_wrap, &it[i])))
            wrap = x;
        else
            return false;
    }
    emit(s, AU_PROP_flex_direction, dir);
    emit(s, AU_PROP_flex_wrap, wrap);
    return true;
}

static bool sh_gap(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 2)
        return false;
    AuCssValue *r = try_lh(s->cx, AU_PROP_row_gap, &it[0]);
    AuCssValue *c = n == 2 ? try_lh(s->cx, AU_PROP_column_gap, &it[1]) : r;
    if (!r || !c)
        return false;
    emit(s, AU_PROP_row_gap, r);
    emit(s, AU_PROP_column_gap, c);
    return true;
}

/* contain-intrinsic-size: one or two of none | <length> | auto <length>
 * (width, then height; one value sets both). */
static bool sh_contain_intrinsic_size(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 4)
        return false;
    AuCssValue *v[2] = {NULL, NULL};
    uint32_t i = 0, k = 0;
    while (i < n && k < 2) {
        if (css_is_kw(&it[i], AU_K_auto_) && i + 1 < n)
            i++;
        v[k] = try_lh(s->cx, k ? AU_PROP_contain_intrinsic_height : AU_PROP_contain_intrinsic_width, &it[i]);
        if (!v[k])
            return false;
        i++;
        k++;
    }
    if (i != n || !v[0])
        return false;
    emit(s, AU_PROP_contain_intrinsic_width, v[0]);
    emit(s, AU_PROP_contain_intrinsic_height, v[1] ? v[1] : v[0]);
    return true;
}

static bool sh_place(Sh *s, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b)
{
    const uint16_t *lh = css_shorthand_longhands(sh);
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 4)
        return false;
    /* split into the align part and the justify part: try each split */
    for (uint32_t k = 1; k <= n; k++) {
        AuCssValue *seq1[2], *seq2[2];
        if (k > 2 || n - k > 2)
            continue;
        for (uint32_t i = 0; i < k; i++)
            seq1[i] = dup(s->cx, &it[i]);
        AuCssValue *al = css_validate_longhand(s->cx, lh[0], list_of(s->cx, seq1, k, AU_CSSV_LIST));
        if (!al)
            continue;
        AuCssValue *ju;
        if (k == n) {
            /* one value sets both, and must be valid for both */
            ju = css_validate_longhand(s->cx, lh[1], dup(s->cx, al));
            if (!ju)
                return false;
        } else {
            for (uint32_t i = k; i < n; i++)
                seq2[i - k] = dup(s->cx, &it[i]);
            ju = css_validate_longhand(s->cx, lh[1], list_of(s->cx, seq2, n - k, AU_CSSV_LIST));
            if (!ju)
                continue;
        }
        emit(s, lh[0], al);
        emit(s, lh[1], ju);
        return true;
    }
    return false;
}

/* ---- Grid ---- */

static AuCssValue *seq_value(CssParseCtx *cx, const AuCssValue *it, uint32_t a, uint32_t b)
{
    AuCssValue *seq[128];
    if (b <= a || b - a > 128)
        return NULL;
    for (uint32_t i = a; i < b; i++)
        seq[i - a] = dup(cx, &it[i]);
    return list_of(cx, seq, b - a, AU_CSSV_LIST);
}

static int find_slash(const AuCssValue *it, uint32_t a, uint32_t n)
{
    for (uint32_t i = a; i < n; i++)
        if (it[i].type == AU_CSSV_DELIM && it[i].unit == '/')
            return (int)i;
    return -1;
}

/* grid-template, also used by grid. Emits rows, columns, areas. */
static bool grid_template(Sh *s, const AuCssValue *it, uint32_t n)
{
    CssParseCtx *cx = s->cx;
    if (n == 1 && css_is_kw(&it[0], AU_K_none)) {
        emit(s, AU_PROP_grid_template_rows, NULL);
        emit(s, AU_PROP_grid_template_columns, NULL);
        emit(s, AU_PROP_grid_template_areas, NULL);
        return true;
    }
    int sl = find_slash(it, 0, n);
    bool strings = false;
    for (uint32_t i = 0; i < (sl < 0 ? n : (uint32_t)sl); i++)
        if (it[i].type == AU_CSSV_STRING)
            strings = true;
    if (!strings) {
        if (sl < 0)
            return false;
        AuCssValue *rows = css_validate_longhand(cx, AU_PROP_grid_template_rows, seq_value(cx, it, 0, (uint32_t)sl));
        AuCssValue *cols = css_validate_longhand(cx, AU_PROP_grid_template_columns, seq_value(cx, it, (uint32_t)sl + 1, n));
        if (!rows || !cols)
            return false;
        emit(s, AU_PROP_grid_template_rows, rows);
        emit(s, AU_PROP_grid_template_columns, cols);
        emit(s, AU_PROP_grid_template_areas, NULL);
        return true;
    }
    /* [ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]? */
    uint32_t end = sl < 0 ? n : (uint32_t)sl;
    AuCssValue *areas[64], *tracks[192];
    uint32_t na = 0, nt = 0;
    uint32_t i = 0;
    while (i < end) {
        if (it[i].type == AU_CSSV_FUNCTION && it[i].kw == 0 && it[i].unit == '[') {
            if (nt >= 190)
                return false;
            tracks[nt++] = dup(cx, &it[i]);
            i++;
        }
        if (i >= end || it[i].type != AU_CSSV_STRING || na >= 64)
            return false;
        areas[na++] = dup(cx, &it[i]);
        i++;
        AuCssValue *size = NULL;
        if (i < end && !(it[i].type == AU_CSSV_STRING) && !(it[i].type == AU_CSSV_FUNCTION && it[i].unit == '[')) {
            size = css_validate_longhand(cx, AU_PROP_grid_auto_rows, dup(cx, &it[i]));
            if (!size)
                return false;
            i++;
        }
        tracks[nt++] = size ? size : css_new_kw(cx->arena, AU_K_auto_);
        if (i < end && it[i].type == AU_CSSV_FUNCTION && it[i].kw == 0 && it[i].unit == '[') {
            tracks[nt++] = dup(cx, &it[i]);
            i++;
        }
    }
    AuCssValue *ar = css_validate_longhand(cx, AU_PROP_grid_template_areas, list_of(cx, areas, na, AU_CSSV_LIST));
    AuCssValue *rows = css_validate_longhand(cx, AU_PROP_grid_template_rows, list_of(cx, tracks, nt, AU_CSSV_LIST));
    if (!ar || !rows)
        return false;
    AuCssValue *cols = NULL;
    if (sl >= 0) {
        cols = css_validate_longhand(cx, AU_PROP_grid_template_columns, seq_value(cx, it, (uint32_t)sl + 1, n));
        if (!cols || css_is_kw(cols, AU_K_none))
            return false;
    }
    emit(s, AU_PROP_grid_template_rows, rows);
    emit(s, AU_PROP_grid_template_columns, cols);
    emit(s, AU_PROP_grid_template_areas, ar);
    return true;
}

static bool sh_grid_template(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    return it && grid_template(s, it, n);
}

static bool sh_grid(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    CssParseCtx *cx = s->cx;
    uint32_t n;
    const AuCssValue *it = sh_items(cx, tk, a, b, &n, false, NULL);
    if (!it)
        return false;
    int sl = find_slash(it, 0, n);
    bool af_left = false, af_right = false;
    for (uint32_t i = 0; i < n; i++)
        if (it[i].type == AU_CSSV_IDENT && it[i].n == 9 && memcmp(it[i].u.str, "auto-flow", 9) == 0) {
            if (sl >= 0 && i > (uint32_t)sl)
                af_right = true;
            else
                af_left = true;
        }
    if (!af_left && !af_right) {
        uint32_t n0 = s->out->n;
        if (!grid_template(s, it, n))
            return false;
        (void)n0;
        emit(s, AU_PROP_grid_auto_rows, NULL);
        emit(s, AU_PROP_grid_auto_columns, NULL);
        emit(s, AU_PROP_grid_auto_flow, NULL);
        return true;
    }
    if (sl < 0 || (af_left && af_right))
        return false;
    /* [auto-flow && dense?] <auto-rows>? / <template-columns>, or the mirror */
    uint32_t fa = af_left ? 0 : (uint32_t)sl + 1, fb = af_left ? (uint32_t)sl : n;
    bool dense = false;
    AuCssValue *sizes[64];
    uint32_t ns = 0;
    for (uint32_t i = fa; i < fb; i++) {
        if (it[i].type == AU_CSSV_IDENT && it[i].n == 9 && memcmp(it[i].u.str, "auto-flow", 9) == 0)
            continue;
        if (css_is_kw(&it[i], AU_K_dense) && !dense) {
            dense = true;
            continue;
        }
        if (ns >= 64)
            return false;
        sizes[ns++] = dup(cx, &it[i]);
    }
    AuCssValue *autos = NULL;
    if (ns) {
        autos = css_validate_longhand(cx, AU_PROP_grid_auto_rows, list_of(cx, sizes, ns, AU_CSSV_LIST));
        if (!autos)
            return false;
    }
    uint32_t ta = af_left ? (uint32_t)sl + 1 : 0, tb = af_left ? n : (uint32_t)sl;
    AuCssValue *tmpl = css_validate_longhand(cx, AU_PROP_grid_template_rows, seq_value(cx, it, ta, tb));
    if (!tmpl)
        return false;
    AuCssValue *flow_items[2];
    uint32_t nf = 0;
    flow_items[nf++] = css_new_kw(cx->arena, af_left ? AU_K_row : AU_K_column);
    if (dense)
        flow_items[nf++] = css_new_kw(cx->arena, AU_K_dense);
    AuCssValue *flow = list_of(cx, flow_items, nf, AU_CSSV_LIST);
    if (af_left) {
        emit(s, AU_PROP_grid_template_rows, NULL);
        emit(s, AU_PROP_grid_template_columns, tmpl);
        emit(s, AU_PROP_grid_auto_rows, autos);
        emit(s, AU_PROP_grid_auto_columns, NULL);
    } else {
        emit(s, AU_PROP_grid_template_rows, tmpl);
        emit(s, AU_PROP_grid_template_columns, NULL);
        emit(s, AU_PROP_grid_auto_rows, NULL);
        emit(s, AU_PROP_grid_auto_columns, autos);
    }
    emit(s, AU_PROP_grid_template_areas, NULL);
    emit(s, AU_PROP_grid_auto_flow, flow);
    return true;
}

static bool is_ident_line(const AuCssValue *v)
{
    return v && (v->type == AU_CSSV_IDENT || (v->type == AU_CSSV_KEYWORD && v->kw != AU_K_auto_ && v->kw != AU_K_span));
}

static bool grid_lines(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b, AuCssValue **lines, uint32_t max,
                       uint32_t *count)
{
    CssParseCtx *cx = s->cx;
    uint32_t n;
    const AuCssValue *it = sh_items(cx, tk, a, b, &n, false, NULL);
    if (!it)
        return false;
    uint32_t k = 0, start = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || (it[i].type == AU_CSSV_DELIM && it[i].unit == '/')) {
            if (k >= max)
                return false;
            AuCssValue *v = css_validate_longhand(cx, AU_PROP_grid_row_start, seq_value(cx, it, start, i));
            if (!v)
                return false;
            lines[k++] = v;
            start = i + 1;
        }
    }
    *count = k;
    return true;
}

static bool sh_grid_row_col(Sh *s, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuCssValue *lines[2];
    uint32_t n;
    if (!grid_lines(s, tk, a, b, lines, 2, &n))
        return false;
    const uint16_t *lh = css_shorthand_longhands(sh);
    AuCssValue *end = n == 2 ? lines[1] : is_ident_line(lines[0]) ? lines[0] : css_new_kw(s->cx->arena, AU_K_auto_);
    emit(s, lh[0], lines[0]);
    emit(s, lh[1], end);
    return true;
}

static bool sh_grid_area(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuCssValue *l[4];
    uint32_t n;
    if (!grid_lines(s, tk, a, b, l, 4, &n))
        return false;
    AuCssValue *autov = css_new_kw(s->cx->arena, AU_K_auto_);
    AuCssValue *rs = l[0];
    AuCssValue *cs = n > 1 ? l[1] : is_ident_line(rs) ? rs : autov;
    AuCssValue *re = n > 2 ? l[2] : is_ident_line(rs) ? rs : autov;
    AuCssValue *ce = n > 3 ? l[3] : is_ident_line(cs) ? cs : autov;
    emit(s, AU_PROP_grid_row_start, rs);
    emit(s, AU_PROP_grid_column_start, cs);
    emit(s, AU_PROP_grid_row_end, re);
    emit(s, AU_PROP_grid_column_end, ce);
    return true;
}

/* ---- Transitions and animations ---- */

bool css_is_easing(const AuCssValue *it);

static bool is_time_v(const AuCssValue *v)
{
    if (v->type == AU_CSSV_DIMENSION)
        return v->unit == AU_UNIT_S || v->unit == AU_UNIT_MS;
    return v->type == AU_CSSV_CALC && css_value_type(v) == CT_TIME;
}

static bool sh_transition(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    CssParseCtx *cx = s->cx;
    AuCssValue *whole = css_parse_value(cx, tk, a, b);
    if (!whole)
        return false;
    uint32_t nl = whole->type == AU_CSSV_COMMA_LIST ? whole->n : 1;
    AuCssValue *out[5];
    for (int p = 0; p < 5; p++)
        out[p] = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, nl);
    for (uint32_t l = 0; l < nl; l++) {
        const AuCssValue *item = whole->type == AU_CSSV_COMMA_LIST ? &whole->u.items[l] : whole;
        uint32_t n;
        const AuCssValue *it = css_items(item, &n);
        AuCssValue *prop = NULL, *dur = NULL, *ease = NULL, *delay = NULL, *beh = NULL;
        for (uint32_t i = 0; i < n; i++) {
            const AuCssValue *c = &it[i];
            if (is_time_v(c)) {
                if (!dur) {
                    if (c->type == AU_CSSV_DIMENSION && c->u.num < 0 && !c->n)
                        return false;
                    dur = dup(cx, c);
                } else if (!delay) {
                    delay = dup(cx, c);
                } else {
                    return false;
                }
            } else if (!ease && css_is_easing(c)) {
                ease = dup(cx, c);
            } else if (!beh && (css_is_kw(c, AU_K_normal) || css_is_kw(c, AU_K_allow_discrete))) {
                beh = dup(cx, c);
            } else if (!prop && (css_is_kw(c, AU_K_none) || css_is_kw(c, AU_K_all) || c->type == AU_CSSV_IDENT ||
                                 (c->type == AU_CSSV_KEYWORD && c->kw != AU_K_initial && c->kw != AU_K_inherit &&
                                  c->kw != AU_K_unset && c->kw != AU_K_default_))) {
                if (css_is_kw(c, AU_K_none) && nl > 1)
                    return false;
                prop = dup(cx, c);
            } else {
                return false;
            }
        }
        AuCssValue *vals[5] = {prop, dur, ease, delay, beh};
        static const uint16_t lh[5] = {AU_PROP_transition_property, AU_PROP_transition_duration,
                                       AU_PROP_transition_timing_function, AU_PROP_transition_delay,
                                       AU_PROP_transition_behavior};
        for (int p = 0; p < 5; p++) {
            uint32_t ni;
            const AuCssValue *init = css_items(css_initial_value(lh[p]), &ni);
            out[p]->u.items[l] = vals[p] ? *vals[p] : init[0];
        }
    }
    emit(s, AU_PROP_transition_property, out[0]->n == 1 && css_is_kw(&out[0]->u.items[0], AU_K_none)
                                             ? &out[0]->u.items[0] : out[0]);
    emit(s, AU_PROP_transition_duration, out[1]);
    emit(s, AU_PROP_transition_timing_function, out[2]);
    emit(s, AU_PROP_transition_delay, out[3]);
    emit(s, AU_PROP_transition_behavior, out[4]);
    return true;
}

static bool sh_animation(Sh *s, const CssTokens *tk, uint32_t a, uint32_t b)
{
    CssParseCtx *cx = s->cx;
    AuCssValue *whole = css_parse_value(cx, tk, a, b);
    if (!whole)
        return false;
    uint32_t nl = whole->type == AU_CSSV_COMMA_LIST ? whole->n : 1;
    static const uint16_t lh[8] = {AU_PROP_animation_name, AU_PROP_animation_duration,
                                   AU_PROP_animation_timing_function, AU_PROP_animation_delay,
                                   AU_PROP_animation_iteration_count, AU_PROP_animation_direction,
                                   AU_PROP_animation_fill_mode, AU_PROP_animation_play_state};
    AuCssValue *out[8];
    for (int p = 0; p < 8; p++)
        out[p] = css_new_list(cx->arena, AU_CSSV_COMMA_LIST, nl);
    for (uint32_t l = 0; l < nl; l++) {
        const AuCssValue *item = whole->type == AU_CSSV_COMMA_LIST ? &whole->u.items[l] : whole;
        uint32_t n;
        const AuCssValue *it = css_items(item, &n);
        AuCssValue *v[8] = {0};
        for (uint32_t i = 0; i < n; i++) {
            const AuCssValue *c = &it[i];
            AuCssValue *x;
            if (is_time_v(c) || css_is_kw(c, AU_K_auto_)) {
                if (!v[1] && (x = try_lh(cx, AU_PROP_animation_duration, c)))
                    v[1] = &x->u.items[0];
                else if (!v[3] && (x = try_lh(cx, AU_PROP_animation_delay, c)))
                    v[3] = &x->u.items[0];
                else
                    return false;
            } else if (!v[2] && css_is_easing(c)) {
                v[2] = dup(cx, c);
            } else if (!v[4] && (css_is_kw(c, AU_K_infinite) || c->type == AU_CSSV_NUMBER)) {
                if (!(x = try_lh(cx, AU_PROP_animation_iteration_count, c)))
                    return false;
                v[4] = &x->u.items[0];
            } else if (!v[5] && (x = try_lh(cx, AU_PROP_animation_direction, c))) {
                v[5] = &x->u.items[0];
            } else if (!v[6] && (x = try_lh(cx, AU_PROP_animation_fill_mode, c))) {
                v[6] = &x->u.items[0];
            } else if (!v[7] && (x = try_lh(cx, AU_PROP_animation_play_state, c))) {
                v[7] = &x->u.items[0];
            } else if (!v[0] && (x = try_lh(cx, AU_PROP_animation_name, c))) {
                v[0] = &x->u.items[0];
            } else {
                return false;
            }
        }
        for (int p = 0; p < 8; p++) {
            uint32_t ni;
            const AuCssValue *init = css_items(css_initial_value(lh[p]), &ni);
            out[p]->u.items[l] = v[p] ? *v[p] : init[0];
        }
    }
    for (int p = 0; p < 8; p++)
        emit(s, lh[p], out[p]);
    return true;
}

/* ---- Flow-relative pairs ---- */

static bool sh_logical_pair(Sh *s, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b)
{
    const uint16_t *lh = css_shorthand_longhands(sh);
    uint32_t n;
    const AuCssValue *it = sh_items(s->cx, tk, a, b, &n, false, NULL);
    if (!it || n < 1 || n > 2)
        return false;
    AuCssValue *x = try_lh(s->cx, lh[0], &it[0]);
    AuCssValue *y = n == 2 ? try_lh(s->cx, lh[1], &it[1]) : x;
    if (!x || !y)
        return false;
    emit(s, lh[0], x);
    emit(s, lh[1], y);
    return true;
}

/* ---- Dispatch ---- */

bool css_parse_shorthand(CssParseCtx *cx, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b,
                         AuCssDeclBlock *out, uint8_t flags)
{
    Sh s = {cx, out, flags};
    switch (sh) {
    case AU_PROP_all:
        return false; /* only CSS-wide keywords */
    case AU_PROP_margin:
    case AU_PROP_padding:
    case AU_PROP_inset:
    case AU_PROP_border_width:
    case AU_PROP_border_style:
    case AU_PROP_border_color:
        return sh_sides(&s, css_shorthand_longhands(sh), tk, a, b);
    case AU_PROP_border:
    case AU_PROP_border_top:
    case AU_PROP_border_right:
    case AU_PROP_border_bottom:
    case AU_PROP_border_left:
    case AU_PROP_border_block:
    case AU_PROP_border_inline:
    case AU_PROP_border_block_start:
    case AU_PROP_border_block_end:
    case AU_PROP_border_inline_start:
    case AU_PROP_border_inline_end:
        return sh_border_list(&s, css_shorthand_longhands(sh), tk, a, b);
    case AU_PROP_border_radius:
        return sh_border_radius(&s, tk, a, b);
    case AU_PROP_outline:
        return sh_outline(&s, tk, a, b);
    case AU_PROP_background:
        return sh_background(&s, tk, a, b, false);
    case AU_PROP_background_position:
        return sh_background_position(&s, tk, a, b, false);
    case AU_PROP_mask:
        return sh_background(&s, tk, a, b, true);
    case AU_PROP_mask_position:
        return sh_background_position(&s, tk, a, b, true);
    case AU_PROP_font:
        return sh_font(&s, tk, a, b);
    case AU_PROP_font_variant:
        return sh_font_variant(&s, tk, a, b);
    case AU_PROP_list_style:
        return sh_list_style(&s, tk, a, b);
    case AU_PROP_text_decoration:
        return sh_text_decoration(&s, tk, a, b);
    case AU_PROP_flex:
        return sh_flex(&s, tk, a, b);
    case AU_PROP_flex_flow:
        return sh_flex_flow(&s, tk, a, b);
    case AU_PROP_gap:
        return sh_gap(&s, tk, a, b);
    case AU_PROP_contain_intrinsic_size:
        return sh_contain_intrinsic_size(&s, tk, a, b);
    case AU_PROP_place_content:
    case AU_PROP_place_items:
    case AU_PROP_place_self:
        return sh_place(&s, sh, tk, a, b);
    case AU_PROP_grid_template:
        return sh_grid_template(&s, tk, a, b);
    case AU_PROP_grid:
        return sh_grid(&s, tk, a, b);
    case AU_PROP_grid_row:
    case AU_PROP_grid_column:
        return sh_grid_row_col(&s, sh, tk, a, b);
    case AU_PROP_grid_area:
        return sh_grid_area(&s, tk, a, b);
    case AU_PROP_transition:
        return sh_transition(&s, tk, a, b);
    case AU_PROP_animation:
        return sh_animation(&s, tk, a, b);
    case AU_PROP_white_space:
        return sh_white_space(&s, tk, a, b);
    case AU_PROP_text_wrap:
        return sh_text_wrap(&s, tk, a, b);
    case AU_PROP_overflow:
        return sh_overflow(&s, tk, a, b);
    case AU_PROP_overscroll_behavior:
        return sh_overscroll(&s, tk, a, b);
    case AU_PROP_container:
        return sh_container(&s, tk, a, b);
    default:
        if (sh >= AU_PROP_margin_block && sh <= AU_PROP_border_inline_color)
            return sh_logical_pair(&s, sh, tk, a, b);
        return false;
    }
}
