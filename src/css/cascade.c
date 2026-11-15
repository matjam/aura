/* The style resolver: rule index, matching, the cascade (CSS Cascade 5:
 * origins, importance, element-attached styles, layers, specificity,
 * order, revert and revert-layer), custom properties with var(), and the
 * document passes.
 *
 * Rules are indexed by the rightmost compound of each selector: by id,
 * else class, else tag, else in the universal list. Each element looks up
 * its id, classes and tag, filters candidates with the ancestor Bloom
 * filter, and matches the rest. Matched declarations get a 64-bit cascade
 * key; for each property the declaration with the largest key wins.
 *
 *   key = level(3) element-attached(1) layer(12) specificity(24) order(24)
 *   level: 0 UA, 1 user, 2 author; important: 4 author, 5 user, 6 UA.
 *   layer: rank among the origin's layers (unlayered highest), reversed
 *   for important declarations; presentational hints sit below all
 *   author layers. */
#include "css_int.h"

#include <stdlib.h>

/* ---- Layers ---- */

typedef struct LayerNode {
    AuAtom name; /* 0 for anonymous */
    int32_t parent, first_child, last_child, next;
    uint16_t rank;
} LayerNode;

typedef struct {
    LayerNode *n;
    uint32_t count, cap;
} LayerTree;

static int32_t layer_new(LayerTree *t, int32_t parent, AuAtom name)
{
    LayerNode *n = CSS_PUSH(t->n, t->count, t->cap);
    memset(n, 0, sizeof *n);
    n->name = name;
    n->parent = parent;
    n->first_child = n->last_child = n->next = -1;
    int32_t id = (int32_t)t->count - 1;
    if (parent >= 0) {
        LayerNode *p = &t->n[parent];
        if (p->last_child >= 0)
            t->n[p->last_child].next = id;
        else
            p->first_child = id;
        p->last_child = id;
    }
    return id;
}

static int32_t layer_child(LayerTree *t, int32_t parent, AuAtom name)
{
    for (int32_t c = t->n[parent].first_child; c >= 0; c = t->n[c].next)
        if (name && t->n[c].name == name)
            return c;
    return layer_new(t, parent, name);
}

static int32_t layer_path(LayerTree *t, int32_t from, const CssLayerName *ln)
{
    if (ln->n == 0)
        return layer_new(t, from, 0);
    int32_t cur = from;
    for (uint32_t i = 0; i < ln->n; i++)
        cur = layer_child(t, cur, ln->parts[i]);
    return cur;
}

/* Post-order ranks starting at 1: sublayers before their parent, the root
 * (unlayered) last. */
static void layer_rank(LayerTree *t)
{
    if (!t->count)
        return;
    uint16_t rank = 1;
    int32_t cur = 0;
    /* descend to the first leaf */
    while (t->n[cur].first_child >= 0)
        cur = t->n[cur].first_child;
    for (;;) {
        t->n[cur].rank = rank < 4000 ? rank++ : 4000;
        if (cur == 0)
            break;
        int32_t nx = t->n[cur].next;
        if (nx >= 0) {
            cur = nx;
            while (t->n[cur].first_child >= 0)
                cur = t->n[cur].first_child;
        } else {
            cur = t->n[cur].parent;
        }
    }
}

/* ---- Rule index ---- */

typedef struct RuleEntry {
    const AuSelector *sel;
    const AuCssDeclBlock *decls;
    uint32_t order;
    uint32_t spec;
    int32_t layer; /* node in the origin's tree, then its rank */
    uint8_t origin;
    uint8_t pseudo;
} RuleEntry;

typedef struct {
    AuAtom *keys;
    uint32_t *start, *count;
    uint32_t size; /* power of two, 0 if empty */
} AtomMap;

typedef struct {
    uint8_t kind; /* 0 id, 1 class, 2 tag, 3 universal */
    AuAtom atom;
    uint32_t entry;
} IndexKey;

typedef struct {
    const AuCssDeclBlock *decls;
    uint64_t key_n, key_i;
} Matched;

typedef struct {
    uint32_t gen;
    uint64_t key;
    const AuCssDecl *decl;
} Winner;

typedef struct {
    char *text;
    uint32_t len, hash;
    AuCssDeclBlock *block;
} InlineEntry;

#define SHARE_SLOTS 64
typedef struct {
    const AuStyle *parent;
    AuAtom tag;
    uint8_t ns;
    uint16_t pseudo_bits;
    uint32_t n;
    uint32_t *entries;
    uint32_t hash;
    AuStyle *style;
} ShareSlot;

struct AuCssResolver {
    AuCssSheet *ua, *ua_quirks;
    AuCssSheet **sheets;
    uint32_t nsheets, sheets_cap;
    AuCssDevice dev;
    AuCssMetricsFn metrics;
    void *metrics_ctx;
    bool index_dirty;
    bool quirks; /* mode the index was built for */
    bool has_has;
    bool styled_once;
    bool full_restyle;

    RuleEntry *entries;
    uint32_t nentries, entries_cap;
    uint32_t *sorted; /* entry indices grouped by key */
    AtomMap maps[3];
    uint32_t uni_start, uni_count;
    LayerTree layers[3];

    CssPropertyRule *props; /* @property, borrowed from sheets */
    uint32_t nprops, props_cap;

    Matched *matched;
    uint32_t nmatched, matched_cap;
    uint32_t *matched_entries;
    uint32_t nme, me_cap;
    Winner winners[AU_PROP_LOGICAL_END];
    uint32_t gen;
    uint16_t touched[AU_PROP_LOGICAL_END];
    uint32_t ntouched;

    InlineEntry *inl;
    uint32_t ninl, inl_cap;

    ShareSlot share[SHARE_SLOTS];
    const AuStyle *root_style;
    CssBloom bloom;
};

static void map_free(AtomMap *m)
{
    au_free(m->keys);
    au_free(m->start);
    au_free(m->count);
    memset(m, 0, sizeof *m);
}

static int key_cmp(const void *a, const void *b)
{
    const IndexKey *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    if (x->atom != y->atom)
        return x->atom < y->atom ? -1 : 1;
    return x->entry < y->entry ? -1 : x->entry > y->entry;
}

static void map_build(AtomMap *m, const IndexKey *keys, uint32_t a, uint32_t b)
{
    uint32_t distinct = 0;
    for (uint32_t i = a; i < b; i++)
        if (i == a || keys[i].atom != keys[i - 1].atom)
            distinct++;
    if (!distinct)
        return;
    uint32_t size = 16;
    while (size < distinct * 2)
        size *= 2;
    m->size = size;
    m->keys = au_calloc(size * sizeof(AuAtom));
    m->start = au_calloc(size * sizeof(uint32_t));
    m->count = au_calloc(size * sizeof(uint32_t));
    for (uint32_t i = a; i < b;) {
        uint32_t j = i;
        while (j < b && keys[j].atom == keys[i].atom)
            j++;
        uint32_t h = (keys[i].atom * 2654435761u) & (size - 1);
        while (m->keys[h])
            h = (h + 1) & (size - 1);
        m->keys[h] = keys[i].atom;
        m->start[h] = i;
        m->count[h] = j - i;
        i = j;
    }
}

static bool map_get(const AtomMap *m, AuAtom a, uint32_t *start, uint32_t *count)
{
    if (!m->size || !a)
        return false;
    uint32_t h = (a * 2654435761u) & (m->size - 1);
    while (m->keys[h]) {
        if (m->keys[h] == a) {
            *start = m->start[h];
            *count = m->count[h];
            return true;
        }
        h = (h + 1) & (m->size - 1);
    }
    return false;
}

/* The index key of a selector: its rightmost compound's id, class or tag. */
static IndexKey selector_key(const AuSelector *s, bool quirks)
{
    IndexKey k = {3, 0, 0};
    for (uint32_t i = 0; i < s->ncomp && s->comp[i].type != SC_COMBINATOR; i++) {
        const AuSelComp *c = &s->comp[i];
        if (c->type == SC_ID) {
            k.kind = 0;
            k.atom = quirks ? css_atom_lower(c->name) : c->name;
            return k;
        }
        if (c->type == SC_CLASS && k.kind > 1) {
            k.kind = 1;
            k.atom = quirks ? css_atom_lower(c->name) : c->name;
        } else if (c->type == SC_TAG && k.kind > 2) {
            k.kind = 2;
            k.atom = c->name;
        }
    }
    return k;
}

typedef struct {
    const CssRule *rule;
    const AuCssSheet *sheet;
    int32_t layer;
} Frame;

static void add_entries(AuCssResolver *r, const AuSelList *sel, const AuCssDeclBlock *decls, uint8_t origin,
                        int32_t layer, uint32_t *order, bool nested_decls)
{
    if (!sel || !decls->n)
        return;
    uint32_t spec_override = nested_decls ? css_sellist_max_spec(sel) : 0;
    uint32_t ord = (*order)++;
    for (uint32_t i = 0; i < sel->n; i++) {
        RuleEntry *e = CSS_PUSH(r->entries, r->nentries, r->entries_cap);
        e->sel = &sel->sel[i];
        e->decls = decls;
        e->order = ord;
        e->spec = nested_decls ? spec_override : sel->sel[i].spec;
        e->layer = layer;
        e->origin = origin;
        e->pseudo = sel->sel[i].pseudo;
        if (sel->sel[i].flags & SELF_HAS)
            r->has_has = true;
    }
}

static void walk_sheet(AuCssResolver *r, const AuCssSheet *sheet, int32_t layer, uint32_t *order, Frame **stack,
                       uint32_t *sn, uint32_t *scap)
{
    if (sheet->media && !css_media_eval(sheet->media, &r->dev))
        return;
    LayerTree *lt = &r->layers[sheet->origin];
    for (uint32_t i = 0; i < sheet->nprops; i++)
        *CSS_PUSH(r->props, r->nprops, r->props_cap) = sheet->props[i];
    /* frames are processed in order: push the first rule */
    uint32_t base = *sn;
    if (sheet->rules) {
        Frame *f = CSS_PUSH(*stack, *sn, *scap);
        f->rule = sheet->rules;
        f->sheet = sheet;
        f->layer = layer;
    }
    while (*sn > base) {
        Frame f = (*stack)[*sn - 1];
        (*sn)--;
        const CssRule *ru = f.rule;
        if (!ru)
            continue;
        /* continue with the next sibling after this rule's children */
        if (ru->next) {
            Frame *nf = CSS_PUSH(*stack, *sn, *scap);
            nf->rule = ru->next;
            nf->sheet = f.sheet;
            nf->layer = f.layer;
        }
        int32_t child_layer = f.layer;
        bool descend = false;
        switch (ru->type) {
        case RULE_STYLE:
            add_entries(r, ru->sel, &ru->decls, f.sheet->origin, f.layer, order, false);
            descend = true;
            break;
        case RULE_NESTED_DECLS:
            add_entries(r, ru->sel, &ru->decls, f.sheet->origin, f.layer, order, true);
            break;
        case RULE_MEDIA:
            descend = css_media_eval(ru->media, &r->dev);
            break;
        case RULE_SUPPORTS:
            descend = ru->ok;
            break;
        case RULE_LAYER_BLOCK:
            child_layer = layer_path(lt, f.layer, &ru->layers[0]);
            descend = true;
            break;
        case RULE_LAYER_STMT:
            for (uint32_t k = 0; k < ru->nlayers; k++)
                layer_path(lt, f.layer, &ru->layers[k]);
            break;
        case RULE_IMPORT: {
            const AuCssImport *im = &f.sheet->imports[ru->index];
            if (!im->sheet || !ru->ok || (ru->media && !css_media_eval(ru->media, &r->dev)))
                break;
            int32_t il = f.layer;
            if (ru->nlayers)
                il = layer_path(lt, f.layer, &ru->layers[0]);
            /* the imported sheet's rules come here in order: walk it now */
            walk_sheet(r, im->sheet, il, order, stack, sn, scap);
            break;
        }
        default:
            break; /* @container, @scope, @starting-style: not applied */
        }
        if (descend && ru->children) {
            Frame *nf = CSS_PUSH(*stack, *sn, *scap);
            nf->rule = ru->children;
            nf->sheet = f.sheet;
            nf->layer = child_layer;
        }
    }
}

static void index_clear(AuCssResolver *r)
{
    r->nentries = 0;
    au_free(r->sorted);
    r->sorted = NULL;
    for (int i = 0; i < 3; i++)
        map_free(&r->maps[i]);
    for (int i = 0; i < 3; i++)
        r->layers[i].count = 0;
    r->uni_start = r->uni_count = 0;
    r->has_has = false;
    r->nprops = 0;
}

static void share_clear(AuCssResolver *r);

static void index_build(AuCssResolver *r, bool quirks)
{
    index_clear(r);
    share_clear(r);
    r->quirks = quirks;
    for (int i = 0; i < 3; i++)
        layer_new(&r->layers[i], -1, 0); /* root: unlayered */
    uint32_t order = 1;
    Frame *stack = NULL;
    uint32_t sn = 0, scap = 0;
    walk_sheet(r, r->ua, 0, &order, &stack, &sn, &scap);
    if (quirks)
        walk_sheet(r, r->ua_quirks, 0, &order, &stack, &sn, &scap);
    for (uint32_t i = 0; i < r->nsheets; i++)
        walk_sheet(r, r->sheets[i], 0, &order, &stack, &sn, &scap);
    au_free(stack);
    for (int i = 0; i < 3; i++)
        layer_rank(&r->layers[i]);
    for (uint32_t i = 0; i < r->nentries; i++)
        r->entries[i].layer = r->layers[r->entries[i].origin].n[r->entries[i].layer].rank;
    IndexKey *keys = au_alloc((r->nentries ? r->nentries : 1) * sizeof(IndexKey));
    for (uint32_t i = 0; i < r->nentries; i++) {
        keys[i] = selector_key(r->entries[i].sel, quirks);
        keys[i].entry = i;
    }
    qsort(keys, r->nentries, sizeof(IndexKey), key_cmp);
    r->sorted = au_alloc((r->nentries ? r->nentries : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < r->nentries; i++)
        r->sorted[i] = keys[i].entry;
    uint32_t a = 0;
    for (int kind = 0; kind < 4; kind++) {
        uint32_t b = a;
        while (b < r->nentries && keys[b].kind == kind)
            b++;
        if (kind < 3)
            map_build(&r->maps[kind], keys, a, b);
        else {
            r->uni_start = a;
            r->uni_count = b - a;
        }
        a = b;
    }
    au_free(keys);
    r->index_dirty = false;
}

/* ---- Resolver lifetime ---- */

AuCssResolver *au_css_resolver_new(void)
{
    au_css_init();
    AuCssResolver *r = au_calloc(sizeof *r);
    au_css_device_default(&r->dev);
    r->ua = css_sheet_parse(css_ua_sheet, strlen(css_ua_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, false);
    r->ua_quirks = css_sheet_parse(css_ua_quirks_sheet, strlen(css_ua_quirks_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, true);
    r->index_dirty = true;
    return r;
}

static void share_clear(AuCssResolver *r)
{
    for (int i = 0; i < SHARE_SLOTS; i++) {
        au_free(r->share[i].entries);
        au_style_release(r->share[i].style);
        memset(&r->share[i], 0, sizeof r->share[i]);
    }
}

static void inline_clear(AuCssResolver *r)
{
    for (uint32_t i = 0; i < r->ninl; i++) {
        au_free(r->inl[i].text);
        au_css_decl_block_free(r->inl[i].block);
    }
    r->ninl = 0;
}

void au_css_resolver_free(AuCssResolver *r)
{
    if (!r)
        return;
    share_clear(r);
    index_clear(r);
    inline_clear(r);
    au_free(r->inl);
    au_free(r->entries);
    for (int i = 0; i < 3; i++)
        au_free(r->layers[i].n);
    au_free(r->props);
    au_free(r->matched);
    au_free(r->matched_entries);
    au_css_sheet_free(r->ua);
    au_css_sheet_free(r->ua_quirks);
    for (uint32_t i = 0; i < r->nsheets; i++)
        au_css_sheet_free(r->sheets[i]);
    au_free(r->sheets);
    au_free(r);
}

void au_css_resolver_set_device(AuCssResolver *r, const AuCssDevice *d)
{
    r->dev = *d;
    r->index_dirty = true;
    r->full_restyle = true;
}

const AuCssDevice *au_css_resolver_device(const AuCssResolver *r) { return &r->dev; }

void au_css_resolver_set_metrics(AuCssResolver *r, AuCssMetricsFn fn, void *ctx)
{
    r->metrics = fn;
    r->metrics_ctx = ctx;
    r->full_restyle = true;
}

void au_css_resolver_add_sheet(AuCssResolver *r, AuCssSheet *s)
{
    *CSS_PUSH(r->sheets, r->nsheets, r->sheets_cap) = s;
    au_css_resolver_sheets_changed(r);
}

void au_css_resolver_remove_sheet(AuCssResolver *r, AuCssSheet *s)
{
    for (uint32_t i = 0; i < r->nsheets; i++)
        if (r->sheets[i] == s) {
            memmove(&r->sheets[i], &r->sheets[i + 1], (r->nsheets - i - 1) * sizeof(AuCssSheet *));
            r->nsheets--;
            /* the index points into s: drop it before freeing */
            index_clear(r);
            share_clear(r);
            au_css_sheet_free(s);
            break;
        }
    au_css_resolver_sheets_changed(r);
}

void au_css_resolver_sheets_changed(AuCssResolver *r)
{
    r->index_dirty = true;
    r->full_restyle = true;
}

static const AuCssKeyframes *find_keyframes(const AuCssSheet *s, AuAtom name, const AuCssKeyframes *best)
{
    for (uint32_t i = 0; i < s->nimports; i++)
        if (s->imports[i].sheet)
            best = find_keyframes(s->imports[i].sheet, name, best);
    for (uint32_t i = 0; i < s->nkeyframes; i++)
        if (s->keyframes[i].name == name)
            best = &s->keyframes[i];
    return best;
}

const AuCssKeyframes *au_css_resolver_keyframes(AuCssResolver *r, AuAtom name)
{
    const AuCssKeyframes *best = NULL;
    for (uint32_t i = 0; i < r->nsheets; i++)
        best = find_keyframes(r->sheets[i], name, best);
    return best;
}

bool au_css_media_matches(AuCssResolver *r, const char *query, size_t len)
{
    AuCssMediaList *m = css_media_parse_text(query, len);
    bool ok = css_media_eval(m, &r->dev);
    css_media_free(m);
    return ok;
}

/* ---- Matching ---- */

static uint64_t make_key(int level, int attached, uint32_t layer, uint32_t spec, uint32_t order)
{
    if (layer > 4095)
        layer = 4095;
    if (order > 0xFFFFFF)
        order = 0xFFFFFF;
    return ((uint64_t)level << 61) | ((uint64_t)attached << 60) | ((uint64_t)layer << 48) |
           ((uint64_t)(spec & 0xFFFFFF) << 24) | order;
}

static void push_matched(AuCssResolver *r, const AuCssDeclBlock *b, uint64_t kn, uint64_t ki)
{
    Matched *m = CSS_PUSH(r->matched, r->nmatched, r->matched_cap);
    m->decls = b;
    m->key_n = kn;
    m->key_i = ki;
}

static void match_bucket(AuCssResolver *r, uint32_t start, uint32_t count, const AuElement *el, const CssMatchCtx *mc,
                         uint8_t want, uint16_t *pseudo_bits)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ei = r->sorted[start + i];
        const RuleEntry *e = &r->entries[ei];
        if (e->pseudo != want) {
            if (want != AU_CSS_PSEUDO_NONE || !pseudo_bits || e->pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT)
                continue;
            if (*pseudo_bits & (1u << e->pseudo))
                continue; /* already known to match */
            if (css_selector_match(e->sel, el, mc))
                *pseudo_bits |= (uint16_t)(1u << e->pseudo);
            continue;
        }
        if (!css_selector_match(e->sel, el, mc))
            continue;
        *CSS_PUSH(r->matched_entries, r->nme, r->me_cap) = ei;
    }
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Collects the matching rule entries of el for pseudo-element want into
 * r->matched_entries (sorted by entry index) and their declaration blocks
 * into r->matched. */
static void collect(AuCssResolver *r, const AuElement *el, const CssMatchCtx *mc, uint8_t want, uint16_t *pseudo_bits)
{
    r->nme = 0;
    uint32_t s, n;
    bool q = r->quirks;
    if (el->id && map_get(&r->maps[0], q ? css_atom_lower(el->id) : el->id, &s, &n))
        match_bucket(r, s, n, el, mc, want, pseudo_bits);
    for (uint32_t i = 0; i < el->nclasses; i++) {
        AuAtom c = q ? css_atom_lower(el->classes[i]) : el->classes[i];
        bool dup = false;
        if (q)
            for (uint32_t k = 0; k < i; k++)
                if (css_atom_lower(el->classes[k]) == c)
                    dup = true;
        if (!dup && map_get(&r->maps[1], c, &s, &n))
            match_bucket(r, s, n, el, mc, want, pseudo_bits);
    }
    AuAtom tag = el->ns == AU_NS_HTML ? el->tag : css_atom_lower(el->tag);
    if (map_get(&r->maps[2], tag, &s, &n))
        match_bucket(r, s, n, el, mc, want, pseudo_bits);
    match_bucket(r, r->uni_start, r->uni_count, el, mc, want, pseudo_bits);
    if (r->nme > 1)
        qsort(r->matched_entries, r->nme, sizeof(uint32_t), u32_cmp);
    r->nmatched = 0;
    for (uint32_t i = 0; i < r->nme; i++) {
        const RuleEntry *e = &r->entries[r->matched_entries[i]];
        int lvl_n = e->origin;
        int lvl_i = e->origin == AU_CSS_ORIGIN_AUTHOR ? 4 : e->origin == AU_CSS_ORIGIN_USER ? 5 : 6;
        uint32_t layer = (uint32_t)e->layer;
        push_matched(r, e->decls, make_key(lvl_n, 0, layer, e->spec, e->order),
                     make_key(lvl_i, 0, 4095 - layer, e->spec, e->order));
    }
}

/* ---- Inline style cache ---- */

static uint32_t str_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static const AuCssDeclBlock *inline_style(AuCssResolver *r, const AuElement *el)
{
    const AuAttr *a = au_element_attr(el, AU_ATOM_style);
    if (!a || !a->len)
        return NULL;
    uint32_t h = str_hash(a->value, a->len);
    for (uint32_t i = 0; i < r->ninl; i++)
        if (r->inl[i].hash == h && r->inl[i].len == a->len && memcmp(r->inl[i].text, a->value, a->len) == 0)
            return r->inl[i].block;
    if (r->ninl >= 2048)
        inline_clear(r);
    InlineEntry *e = CSS_PUSH(r->inl, r->ninl, r->inl_cap);
    e->text = au_strndup(a->value, a->len);
    e->len = a->len;
    e->hash = h;
    e->block = au_calloc(sizeof(AuCssDeclBlock));
    CssTokens tk = {0};
    css_tokenize(a->value, a->len, &tk);
    const char *base = el->node.doc ? el->node.doc->url : NULL;
    css_parse_decl_list(&tk, 0, tk.n, e->block, r->quirks, base);
    css_tokens_free(&tk);
    return e->block;
}

/* ---- var() substitution ---- */

#define MAX_SUBST_TOKENS 65536

typedef struct {
    AuCssResolver *r;
    const AuCustomProps *custom;
    /* custom properties declared on this element, being resolved */
    struct CssCustomEntry *own;
    uint32_t nown;
    uint8_t *state; /* 0 unresolved, 1 in progress, 2 done */
    int depth;
} VarCtx;

static CssTokList *resolve_custom(VarCtx *vc, AuAtom name);

/* Appends tokens [a, b) of src to out, replacing var() and env(). Returns
 * false if a reference is invalid with no fallback. */
static bool substitute(VarCtx *vc, const CssTokens *src, uint32_t a, uint32_t b, CssTokens *out)
{
    if (++vc->depth > 64) {
        vc->depth--;
        return false;
    }
    bool ok = true;
    uint32_t i = a;
    while (i < b && ok) {
        const CssToken *t = &src->t[i];
        if (t->type == TK_FUNCTION && (t->kw == AU_K_var || t->kw == AU_K_env)) {
            uint32_t end = t->len > b ? b : t->len;
            uint32_t ie = (end > i + 1 && src->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
            uint32_t k = i + 1;
            while (k < ie && src->t[k].type == TK_WS)
                k++;
            if (k >= ie || src->t[k].type != TK_IDENT) {
                ok = false;
                break;
            }
            const CssToken *nm = &src->t[k];
            uint32_t q = k + 1;
            while (q < ie && src->t[q].type == TK_WS)
                q++;
            bool has_fb = q < ie && src->t[q].type == TK_COMMA;
            if (q < ie && !has_fb) {
                ok = false;
                break;
            }
            CssTokList *val = NULL;
            if (t->kw == AU_K_var) {
                const char *s = css_tok_str(src, nm);
                if (!css_ident_is_custom_prop(s, nm->len)) {
                    ok = false;
                    break;
                }
                val = resolve_custom(vc, au_atom(s, nm->len));
            }
            if (val) {
                css_tokens_append(out, &val->tk, 0, val->tk.n);
            } else if (has_fb) {
                ok = substitute(vc, src, q + 1, ie, out);
            } else {
                ok = false;
            }
            i = end;
            continue;
        }
        /* copy one token; block ends are fixed by css_tokens_rematch */
        css_tokens_append(out, src, i, i + 1);
        i++;
        if (out->n > MAX_SUBST_TOKENS)
            ok = false;
    }
    vc->depth--;
    return ok;
}

static CssTokList *substitute_list(VarCtx *vc, const CssTokList *in)
{
    CssTokList *l = au_calloc(sizeof *l);
    l->refs = 1;
    if (!substitute(vc, &in->tk, 0, in->tk.n, &l->tk)) {
        css_toklist_release(l);
        return NULL;
    }
    css_tokens_rematch(&l->tk);
    /* trim whitespace */
    uint32_t s = 0, e = l->tk.n;
    while (s < e && l->tk.t[s].type == TK_WS)
        s++;
    while (e > s && l->tk.t[e - 1].type == TK_WS)
        e--;
    if (s || e != l->tk.n) {
        CssTokList *t = css_toklist_new(&l->tk, s, e);
        css_toklist_release(l);
        l = t;
    }
    for (uint32_t i = 0; i < l->tk.n; i++)
        if (l->tk.t[i].type == TK_FUNCTION && l->tk.t[i].kw == AU_K_attr)
            l->has_attr = 1;
    l->has_var = 0;
    return l;
}

static CssTokList *resolve_custom(VarCtx *vc, AuAtom name)
{
    for (uint32_t i = 0; i < vc->nown; i++) {
        if (vc->own[i].name != name)
            continue;
        if (vc->state[i] == 2)
            return vc->own[i].value;
        if (vc->state[i] == 1) {
            vc->state[i] = 3; /* cycle */
            return NULL;
        }
        if (vc->state[i] == 3)
            return NULL;
        CssTokList *v = vc->own[i].value;
        if (v && v->has_var) {
            vc->state[i] = 1;
            CssTokList *s = substitute_list(vc, v);
            bool cyc = vc->state[i] == 3;
            css_toklist_release(vc->own[i].value);
            vc->own[i].value = cyc ? (css_toklist_release(s), NULL) : s;
        }
        vc->state[i] = vc->state[i] == 3 ? 3 : 2;
        return vc->state[i] == 2 ? vc->own[i].value : NULL;
    }
    return css_custom_get(vc->custom, name);
}

/* ---- The cascade for one element ---- */

typedef struct {
    AuCssResolver *r;
    CssComputeCtx cc;
    const AuElement *el;
    AuCustomProps *custom;
    const AuCssDeclBlock *hints;
    /* cached shorthand expansion for var() */
    const CssTokList *sh_src;
    uint16_t sh_prop;
    AuCssDeclBlock sh_out;
    bool sh_ok;
} Cascade;

static int level_origin(uint64_t key)
{
    int lvl = (int)(key >> 61);
    return lvl <= 2 ? lvl : 6 - lvl;
}

static uint16_t phys_of(Cascade *cs, uint16_t prop)
{
    const AuStyleInh *inh = cs->cc.style->inh;
    return css_logical_to_physical(prop, inh->writing_mode, inh->direction);
}

/* The best declaration for prop (physical) below a threshold. kind 1:
 * origin lower than the origin of thr; kind 2: key >> 48 lower. */
static const AuCssDecl *best_below(Cascade *cs, uint16_t prop, uint64_t thr, int kind, uint64_t *out_key)
{
    AuCssResolver *r = cs->r;
    const AuCssDecl *best = NULL;
    uint64_t bk = 0;
    for (uint32_t m = 0; m < r->nmatched; m++) {
        const Matched *mt = &r->matched[m];
        for (uint32_t i = 0; i < mt->decls->n; i++) {
            const AuCssDecl *d = &mt->decls->d[i];
            if (d->prop == AU_PROP_CUSTOM)
                continue;
            uint16_t p = d->prop < AU_PROP_LONGHAND_END ? d->prop : phys_of(cs, d->prop);
            if (p != prop)
                continue;
            uint64_t k = (d->flags & DF_IMPORTANT) ? mt->key_i : mt->key_n;
            bool below = kind == 1 ? level_origin(k) < level_origin(thr) : (k >> 48) < (thr >> 48);
            if (below && (!best || k >= bk)) {
                best = d;
                bk = k;
            }
        }
    }
    *out_key = bk;
    return best;
}

static void apply_decl(Cascade *cs, uint16_t prop, const AuCssDecl *d, uint64_t key);

static void apply_wide(Cascade *cs, uint16_t prop, uint16_t kw, uint64_t key)
{
    CssComputeCtx *c = &cs->cc;
    bool inherited = (css_props[prop].flags & F_INH) != 0;
    switch (kw) {
    case AU_K_inherit:
        css_compute_inherit(c, prop);
        return;
    case AU_K_initial:
        css_compute_initial(c, prop);
        return;
    case AU_K_revert:
    case AU_K_revert_layer: {
        uint64_t k2;
        const AuCssDecl *d = best_below(cs, prop, key, kw == AU_K_revert ? 1 : 2, &k2);
        if (d) {
            apply_decl(cs, prop, d, k2);
            return;
        }
        break; /* nothing below: unset */
    }
    default:
        break;
    }
    if (inherited)
        css_compute_inherit(c, prop);
    else
        css_compute_initial(c, prop);
}

/* The value of a DK_VAR declaration after substitution, as a temporary
 * declaration in cs->sh_out; NULL if invalid at computed-value time. */
static const AuCssDecl *var_decl(Cascade *cs, uint16_t prop, const AuCssDecl *d)
{
    uint16_t parse_prop = d->aux ? (uint16_t)d->aux : d->prop;
    if (cs->sh_src != d->u.toks || cs->sh_prop != parse_prop) {
        css_decl_block_clear(&cs->sh_out);
        cs->sh_src = d->u.toks;
        cs->sh_prop = parse_prop;
        cs->sh_ok = false;
        VarCtx vc = {cs->r, cs->custom, NULL, 0, NULL, 0};
        CssTokList *sub = substitute_list(&vc, d->u.toks);
        if (sub) {
            const char *base = cs->el && cs->el->node.doc ? cs->el->node.doc->url : NULL;
            cs->sh_ok = css_parse_property(parse_prop, &sub->tk, 0, sub->tk.n, false, &cs->sh_out, cs->r->quirks, base);
            css_toklist_release(sub);
        }
    }
    if (!cs->sh_ok)
        return NULL;
    for (uint32_t i = 0; i < cs->sh_out.n; i++) {
        const AuCssDecl *x = &cs->sh_out.d[i];
        uint16_t p = x->prop < AU_PROP_LONGHAND_END ? x->prop : phys_of(cs, x->prop);
        if (x->prop == d->prop || p == prop)
            return x;
    }
    return NULL;
}

static void apply_decl(Cascade *cs, uint16_t prop, const AuCssDecl *d, uint64_t key)
{
    switch (d->kind) {
    case DK_VALUE:
        if (!css_compute_apply(&cs->cc, prop, &d->u.v, NULL))
            apply_wide(cs, prop, AU_K_unset, key);
        return;
    case DK_BLOB:
        if (!css_compute_apply(&cs->cc, prop, &d->u.blob->value, d->u.blob))
            apply_wide(cs, prop, AU_K_unset, key);
        return;
    case DK_WIDE:
        apply_wide(cs, prop, d->u.v.kw, key);
        return;
    case DK_VAR: {
        const AuCssDecl *x = var_decl(cs, prop, d);
        if (!x) {
            apply_wide(cs, prop, AU_K_unset, key);
            return;
        }
        AuCssDecl copy = *x;
        copy.flags = d->flags;
        if (copy.kind == DK_VAR) {
            apply_wide(cs, prop, AU_K_unset, key);
            return;
        }
        if (copy.kind == DK_BLOB)
            au_css_blob_retain(copy.u.blob);
        apply_decl(cs, prop, &copy, key);
        if (copy.kind == DK_BLOB)
            au_css_blob_release(copy.u.blob);
        return;
    }
    default:
        return;
    }
}

/* Custom property cascade: winners by name, then var() resolution. */
typedef struct {
    AuAtom name;
    uint64_t key;
    const AuCssDecl *d;
} CustomWin;

static int cw_cmp(const void *a, const void *b)
{
    const CustomWin *x = a, *y = b;
    if (x->name != y->name)
        return x->name < y->name ? -1 : 1;
    return x->key < y->key ? -1 : x->key > y->key;
}

static const CssPropertyRule *registered(AuCssResolver *r, AuAtom name)
{
    const CssPropertyRule *p = NULL;
    for (uint32_t i = 0; i < r->nprops; i++)
        if (r->props[i].name == name)
            p = &r->props[i];
    return p;
}

static AuCustomProps *build_custom(Cascade *cs, const AuCustomProps *parent)
{
    AuCssResolver *r = cs->r;
    CustomWin *w = NULL;
    uint32_t nw = 0, wcap = 0;
    for (uint32_t m = 0; m < r->nmatched; m++) {
        const Matched *mt = &r->matched[m];
        for (uint32_t i = 0; i < mt->decls->n; i++) {
            const AuCssDecl *d = &mt->decls->d[i];
            if (d->prop != AU_PROP_CUSTOM)
                continue;
            CustomWin *x = CSS_PUSH(w, nw, wcap);
            x->name = d->aux;
            x->key = (d->flags & DF_IMPORTANT) ? mt->key_i : mt->key_n;
            x->d = d;
        }
    }
    /* non-inherited registered properties do not come from the parent */
    bool parent_filtered = false;
    for (uint32_t i = 0; i < r->nprops && parent; i++)
        if (!r->props[i].inherits && css_custom_get(parent, r->props[i].name))
            parent_filtered = true;
    bool need_initials = false;
    for (uint32_t i = 0; i < r->nprops; i++)
        if (r->props[i].initial && (!parent || !css_custom_get(parent, r->props[i].name) || !r->props[i].inherits))
            need_initials = true;
    if (!nw && !parent_filtered && !need_initials) {
        au_free(w);
        return css_custom_retain((AuCustomProps *)parent);
    }
    qsort(w, nw, sizeof *w, cw_cmp);
    /* one winner per name: the last of each run */
    uint32_t k = 0;
    for (uint32_t i = 0; i < nw; i++) {
        if (k && w[k - 1].name == w[i].name)
            w[k - 1] = w[i];
        else
            w[k++] = w[i];
    }
    nw = k;
    /* merge with the parent's map into a new sorted map */
    AuCustomProps *c = au_calloc(sizeof *c);
    c->refs = 1;
    uint32_t pn = parent ? parent->n : 0;
    c->e = au_calloc((pn + nw + r->nprops + 1) * sizeof(*c->e));
    struct CssCustomEntry *own = au_calloc((nw + 1) * sizeof(*own));
    uint32_t nown = 0;
    uint32_t pi = 0, wi = 0;
    while (pi < pn || wi < nw) {
        bool take_parent = wi >= nw || (pi < pn && parent->e[pi].name < w[wi].name);
        bool both = pi < pn && wi < nw && parent->e[pi].name == w[wi].name;
        if (take_parent && !both) {
            const CssPropertyRule *reg = registered(r, parent->e[pi].name);
            if (!reg || reg->inherits) {
                c->e[c->n].name = parent->e[pi].name;
                c->e[c->n].value = css_toklist_retain(parent->e[pi].value);
                c->n++;
            }
            pi++;
            continue;
        }
        const AuCssDecl *d = w[wi].d;
        AuAtom name = w[wi].name;
        CssTokList *pv = both ? parent->e[pi].value : NULL;
        CssTokList *val = NULL;
        if (d->kind == DK_CUSTOM) {
            /* a value that is only a CSS-wide keyword */
            const CssTokList *t = d->u.toks;
            uint16_t kw = (t->tk.n == 1 && t->tk.t[0].type == TK_IDENT) ? t->tk.t[0].kw : 0;
            if (kw == AU_K_initial)
                val = NULL;
            else if (kw == AU_K_inherit || kw == AU_K_unset || kw == AU_K_revert || kw == AU_K_revert_layer)
                val = css_toklist_retain(pv);
            else
                val = css_toklist_retain(d->u.toks);
        }
        own[nown].name = name;
        own[nown].value = val;
        nown++;
        if (both)
            pi++;
        wi++;
    }
    /* resolve var() in this element's own declarations */
    uint8_t *state = au_calloc(nown + 1);
    VarCtx vc = {r, NULL, own, nown, state, 0};
    /* lookups of names not declared here go to the merged parent values */
    AuCustomProps tmp = {1, c->n, c->e};
    vc.custom = &tmp;
    for (uint32_t i = 0; i < nown; i++)
        resolve_custom(&vc, own[i].name);
    /* merge own into c (both sorted by name) */
    struct CssCustomEntry *merged = au_calloc((c->n + nown + r->nprops + 1) * sizeof(*merged));
    uint32_t mn = 0, a = 0, b = 0;
    while (a < c->n || b < nown) {
        if (b >= nown || (a < c->n && c->e[a].name < own[b].name)) {
            merged[mn++] = c->e[a++];
        } else {
            if (state[b] == 3) {
                css_toklist_release(own[b].value);
                own[b].value = NULL;
            }
            if (own[b].value) {
                merged[mn].name = own[b].name;
                merged[mn].value = own[b].value;
                mn++;
            }
            b++;
        }
    }
    au_free(c->e);
    c->e = merged;
    c->n = mn;
    /* registered initial values for names still missing */
    for (uint32_t i = 0; i < r->nprops; i++) {
        if (!r->props[i].initial || css_custom_get(c, r->props[i].name))
            continue;
        uint32_t pos = 0;
        while (pos < c->n && c->e[pos].name < r->props[i].name)
            pos++;
        memmove(&c->e[pos + 1], &c->e[pos], (c->n - pos) * sizeof(*c->e));
        c->e[pos].name = r->props[i].name;
        c->e[pos].value = css_toklist_retain(r->props[i].initial);
        c->n++;
    }
    au_free(state);
    au_free(own);
    au_free(w);
    if (!c->n) {
        css_custom_release(c);
        return NULL;
    }
    return c;
}

/* Properties computed first, in this order. */
static const uint16_t priority_props[] = {AU_PROP_direction, AU_PROP_writing_mode, AU_PROP_font_size,
                                          AU_PROP_line_height};

static bool is_priority(uint16_t p)
{
    for (size_t i = 0; i < CSS_ARRAY_LEN(priority_props); i++)
        if (priority_props[i] == p)
            return true;
    return false;
}

static void consider(AuCssResolver *r, uint16_t p, uint64_t key, const AuCssDecl *d)
{
    Winner *w = &r->winners[p];
    if (w->gen != r->gen) {
        w->gen = r->gen;
        w->key = key;
        w->decl = d;
        r->touched[r->ntouched++] = p;
    } else if (key >= w->key) {
        w->key = key;
        w->decl = d;
    }
}

static void share_groups(AuStyle *s, const AuStyle *parent, const AuStyle *prev);

/* Runs the cascade over r->matched (plus inline style and hints) and
 * computes the style. */
static AuStyle *cascade(AuCssResolver *r, const AuElement *el, const AuStyle *parent, const AuCssDeclBlock *inl,
                        const AuCssDeclBlock *hints, uint8_t pseudo)
{
    if (hints && hints->n)
        push_matched(r, hints, make_key(2, 0, 0, 0, 0), make_key(4, 0, 4095, 0, 0));
    if (inl && inl->n)
        push_matched(r, inl, make_key(2, 1, 4095, 0, 0), make_key(4, 1, 4095, 0, 0));

    AuStyle *s = css_style_new();
    s->pseudo = pseudo;
    const AuStyle *ip = parent ? parent : &css_initial_style;
    s->inh = (AuStyleInh *)ip->inh;
    s->inh->refs++;
    s->inhr = (AuStyleInhRare *)ip->inhr;
    s->inhr->refs++;
    s->box = css_initial_style.box;
    s->box->refs++;
    s->surr = css_initial_style.surr;
    s->surr->refs++;
    s->bg = css_initial_style.bg;
    s->bg->refs++;
    s->flex = css_initial_style.flex;
    s->flex->refs++;
    s->rare = css_initial_style.rare;
    s->rare->refs++;

    Cascade cs;
    memset(&cs, 0, sizeof cs);
    cs.r = r;
    cs.el = el;
    cs.hints = hints;
    cs.cc.style = s;
    cs.cc.parent = parent;
    cs.cc.dev = &r->dev;
    cs.cc.root = r->root_style && r->root_style != s ? r->root_style : NULL;
    cs.cc.metrics = r->metrics;
    cs.cc.metrics_ctx = r->metrics_ctx;
    cs.cc.el = el;
    cs.cc.is_root = el && !pseudo && el->node.parent && el->node.parent->type == AU_NODE_DOCUMENT;
    cs.cc.quirks = r->quirks;
    cs.cc.ex = cs.cc.ch = -1;
    if (cs.cc.is_root)
        cs.cc.root = NULL;

    /* custom properties */
    AuCustomProps *custom = build_custom(&cs, parent ? parent->inh->custom : NULL);
    cs.custom = custom;
    if (custom != s->inh->custom) {
        AuStyleInh *inh = css_mut_group(s, G_INH);
        css_custom_release(inh->custom);
        inh->custom = custom;
    } else {
        css_custom_release(custom);
    }
    cs.custom = s->inh->custom;

    /* winners */
    r->gen++;
    if (r->gen == 0)
        r->gen = 1;
    r->ntouched = 0;
    for (uint32_t m = 0; m < r->nmatched; m++) {
        const Matched *mt = &r->matched[m];
        for (uint32_t i = 0; i < mt->decls->n; i++) {
            const AuCssDecl *d = &mt->decls->d[i];
            if (d->prop == AU_PROP_CUSTOM || d->prop >= AU_PROP_LOGICAL_END)
                continue;
            consider(r, d->prop, (d->flags & DF_IMPORTANT) ? mt->key_i : mt->key_n, d);
        }
    }
    /* priority properties */
    for (size_t k = 0; k < CSS_ARRAY_LEN(priority_props); k++) {
        uint16_t p = priority_props[k];
        if (r->winners[p].gen == r->gen)
            apply_decl(&cs, p, r->winners[p].decl, r->winners[p].key);
    }
    /* flow-relative properties compete with their physical ones */
    uint32_t nt = r->ntouched;
    for (uint32_t t = 0; t < nt; t++) {
        uint16_t p = r->touched[t];
        if (p < AU_PROP_LONGHAND_END)
            continue;
        uint16_t ph = phys_of(&cs, p);
        consider(r, ph, r->winners[p].key, r->winners[p].decl);
    }
    for (uint32_t t = 0; t < r->ntouched; t++) {
        uint16_t p = r->touched[t];
        if (p >= AU_PROP_LONGHAND_END || is_priority(p))
            continue;
        apply_decl(&cs, p, r->winners[p].decl, r->winners[p].key);
    }
    css_decl_block_clear(&cs.sh_out);

    uint16_t pdisp = parent ? parent->box->display : AU_DISPLAY_BLOCK;
    if (pseudo)
        pdisp = AU_DISPLAY_BLOCK; /* pseudo-elements are not flex items of their originating element here */
    css_compute_finish(&cs.cc, pdisp);
    if (cs.cc.is_root)
        s->flags |= AU_STYLE_F_ROOT;
    if (cs.cc.flags & 2)
        s->flags |= AU_STYLE_F_VIEWPORT_UNITS;
    if (cs.cc.flags & 4)
        s->flags |= AU_STYLE_F_HAS_ATTR;
    share_groups(s, parent, NULL);
    return s;
}

/* Replaces groups equal to the parent's (inherited) or the initial ones
 * (non-inherited) by shared references. */
static void swap_group(AuStyle *s, int g, void *other)
{
    void **slot;
    switch (g) {
    case G_INH: slot = (void **)&s->inh; break;
    case G_INHR: slot = (void **)&s->inhr; break;
    case G_BOX: slot = (void **)&s->box; break;
    case G_SURR: slot = (void **)&s->surr; break;
    case G_BG: slot = (void **)&s->bg; break;
    case G_FLEX: slot = (void **)&s->flex; break;
    default: slot = (void **)&s->rare; break;
    }
    if (*slot == other || !other)
        return;
    if (css_group_equal(g, *slot, other)) {
        (*(uint32_t *)other)++;
        css_group_release(g, *slot);
        *slot = other;
    }
}

static void share_groups(AuStyle *s, const AuStyle *parent, const AuStyle *prev)
{
    const AuStyle *ip = parent ? parent : &css_initial_style;
    swap_group(s, G_INH, ip->inh);
    swap_group(s, G_INHR, ip->inhr);
    swap_group(s, G_BOX, css_initial_style.box);
    swap_group(s, G_SURR, css_initial_style.surr);
    swap_group(s, G_BG, css_initial_style.bg);
    swap_group(s, G_FLEX, css_initial_style.flex);
    swap_group(s, G_RARE, css_initial_style.rare);
    if (prev) {
        for (int g = 0; g < G_COUNT; g++)
            swap_group(s, g, css_style_group(prev, g));
    }
}

/* ---- Style sharing ---- */

static uint32_t share_hash(const AuStyle *parent, const AuElement *el, uint16_t bits, const uint32_t *e, uint32_t n)
{
    uint32_t h = (uint32_t)(uintptr_t)parent * 2654435761u ^ el->tag * 40503u ^ el->ns ^ ((uint32_t)bits << 7);
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ e[i]) * 16777619u;
    return h;
}

static bool share_eligible(const AuElement *el, const AuCssDeclBlock *inl, const AuCssDeclBlock *hints, bool root)
{
    (void)el;
    return !root && !(inl && inl->n) && !(hints && hints->n);
}

/* ---- Element styles ---- */

static void prepare(AuCssResolver *r, const AuElement *el)
{
    bool quirks = el->node.doc && el->node.doc->quirks == AU_QUIRKS_FULL;
    if (r->index_dirty || quirks != r->quirks)
        index_build(r, quirks);
}

static AuStyle *compute_element(AuCssResolver *r, AuElement *el, const AuStyle *parent, const CssBloom *bloom)
{
    prepare(r, el);
    CssMatchCtx mc = {0};
    mc.bloom = bloom;
    mc.quirks = r->quirks;
    uint16_t bits = 0;
    collect(r, el, &mc, AU_CSS_PSEUDO_NONE, &bits);
    const AuCssDeclBlock *inl = inline_style(r, el);
    AuCssDeclBlock hints;
    css_decl_block_init(&hints);
    css_presentational_hints(el, &hints, r->quirks);
    bool root = el->node.parent && el->node.parent->type == AU_NODE_DOCUMENT;
    bool eligible = share_eligible(el, inl, &hints, root);
    uint32_t h = 0;
    ShareSlot *slot = NULL;
    if (eligible) {
        h = share_hash(parent, el, bits, r->matched_entries, r->nme);
        slot = &r->share[h % SHARE_SLOTS];
        if (slot->style && slot->hash == h && slot->parent == parent && slot->tag == el->tag && slot->ns == el->ns &&
            slot->pseudo_bits == bits && slot->n == r->nme &&
            memcmp(slot->entries, r->matched_entries, r->nme * sizeof(uint32_t)) == 0) {
            css_decl_block_clear(&hints);
            return au_style_retain(slot->style);
        }
    }
    AuStyle *s = cascade(r, el, parent, inl, &hints, AU_CSS_PSEUDO_NONE);
    s->pseudo_bits = bits;
    css_decl_block_clear(&hints);
    if (eligible && !(s->flags & AU_STYLE_F_HAS_ATTR)) {
        au_free(slot->entries);
        au_style_release(slot->style);
        slot->parent = parent;
        slot->tag = el->tag;
        slot->ns = el->ns;
        slot->pseudo_bits = bits;
        slot->n = r->nme;
        slot->entries = au_alloc((r->nme ? r->nme : 1) * sizeof(uint32_t));
        memcpy(slot->entries, r->matched_entries, r->nme * sizeof(uint32_t));
        slot->hash = h;
        slot->style = au_style_retain(s);
    }
    return s;
}

AuStyle *au_css_compute(AuCssResolver *r, AuElement *el, const AuStyle *parent)
{
    bool root = el->node.parent && el->node.parent->type == AU_NODE_DOCUMENT;
    if (root)
        r->root_style = NULL;
    else if (!r->root_style && el->node.doc) {
        AuElement *de = au_document_element(el->node.doc);
        r->root_style = de ? au_element_style(de) : NULL;
    }
    AuStyle *s = compute_element(r, el, parent, NULL);
    if (root)
        r->root_style = s;
    return s;
}

AuStyle *au_css_compute_pseudo(AuCssResolver *r, AuElement *el, const AuStyle *el_style, AuCssPseudo p)
{
    prepare(r, el);
    CssMatchCtx mc = {0};
    mc.quirks = r->quirks;
    collect(r, el, &mc, (uint8_t)p, NULL);
    if (!r->nmatched && p != AU_CSS_PSEUDO_MARKER)
        return NULL;
    AuStyle *s = cascade(r, el, el_style, NULL, NULL, (uint8_t)p);
    if (p == AU_CSS_PSEUDO_BEFORE || p == AU_CSS_PSEUDO_AFTER) {
        const AuCssBlob *c = s->rare->content;
        if (!c || css_is_kw(&c->value, AU_K_normal) || css_is_kw(&c->value, AU_K_none) ||
            s->box->display == AU_DISPLAY_NONE) {
            au_style_release(s);
            return NULL;
        }
    }
    return s;
}

/* ---- Document passes ---- */

static void attach_pseudos(AuCssResolver *r, AuElement *el, AuStyle *s)
{
    if (s->pseudos || s->pseudo)
        return;
    static const uint8_t kinds[] = {AU_CSS_PSEUDO_BEFORE, AU_CSS_PSEUDO_AFTER, AU_CSS_PSEUDO_MARKER};
    AuStyle **tail = &s->pseudos;
    for (size_t i = 0; i < CSS_ARRAY_LEN(kinds); i++) {
        uint8_t k = kinds[i];
        if (k == AU_CSS_PSEUDO_MARKER ? !au_display_list_item(s->box->display) : !(s->pseudo_bits & (1u << k)))
            continue;
        AuStyle *p = au_css_compute_pseudo(r, el, s, (AuCssPseudo)k);
        if (p) {
            *tail = p;
            tail = &p->next;
        }
    }
}

static void store_style(AuElement *el, AuStyle *s)
{
    au_style_release((AuStyle *)el->style);
    el->style = s;
}

typedef struct {
    AuElement *el;
    bool forced;
} PathEntry;

/* Styles elements in preorder. With only_dirty, an element is restyled if
 * it or an ancestor is marked dirty; otherwise every element is. */
static void style_pass(AuCssResolver *r, AuDocument *doc, bool only_dirty)
{
    au_css_init();
    AuElement *de = au_document_element(doc);
    if (!de)
        return;
    prepare(r, de);
    share_clear(r);
    css_bloom_clear(&r->bloom);
    r->root_style = NULL;
    PathEntry *path = NULL;
    uint32_t np = 0, pcap = 0;
    const AuNode *root = &doc->node;
    for (AuNode *n = root->first_child; n;) {
        if (n->type != AU_NODE_ELEMENT) {
            n = au_node_next(n, root);
            continue;
        }
        AuElement *el = (AuElement *)n;
        /* pop to the parent */
        while (np && &path[np - 1].el->node != n->parent) {
            css_bloom_pop(&r->bloom, path[np - 1].el, r->quirks);
            np--;
        }
        const AuElement *pe = np ? path[np - 1].el : NULL;
        bool forced = !only_dirty || (np && path[np - 1].forced) || (el->state & AU_STATE_STYLE_DIRTY) || !el->style;
        if (forced) {
            const AuStyle *ps = pe ? au_element_style(pe) : NULL;
            AuStyle *s = compute_element(r, el, ps, &r->bloom);
            if (!pe)
                r->root_style = s;
            store_style(el, s);
            attach_pseudos(r, el, s);
        } else if (!pe) {
            r->root_style = au_element_style(el);
        }
        el->state &= ~(uint32_t)(AU_STATE_STYLE_DIRTY | AU_STATE_SUBTREE_DIRTY);
        bool descend = n->first_child != NULL &&
                       (!only_dirty || forced || (el->state & AU_STATE_SUBTREE_DIRTY) || true);
        if (descend) {
            PathEntry *pe2 = CSS_PUSH(path, np, pcap);
            pe2->el = el;
            pe2->forced = forced && only_dirty ? true : forced;
            css_bloom_push(&r->bloom, el, r->quirks);
            n = n->first_child;
        } else {
            n = au_node_next_skip(n, root);
        }
    }
    au_free(path);
    share_clear(r);
    r->styled_once = true;
    r->full_restyle = false;
}

void au_css_style_document(AuCssResolver *r, AuDocument *doc) { style_pass(r, doc, false); }

void au_css_restyle(AuCssResolver *r, AuDocument *doc)
{
    if (!r->styled_once || r->full_restyle || r->index_dirty)
        style_pass(r, doc, false);
    else
        style_pass(r, doc, true);
}

void au_css_clear_document(AuDocument *doc)
{
    const AuNode *root = &doc->node;
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root))
        if (n->type == AU_NODE_ELEMENT) {
            AuElement *el = (AuElement *)n;
            au_style_release((AuStyle *)el->style);
            el->style = NULL;
        }
}

void au_css_invalidate(AuCssResolver *r, AuElement *el, AuCssChange change, AuAtom attr)
{
    (void)attr;
    if (change == AU_CSS_CHANGE_ALL || r->has_has) {
        r->full_restyle = true;
        return;
    }
    if (change == AU_CSS_CHANGE_INLINE_STYLE)
        inline_clear(r);
    /* the element, and following siblings (sibling combinators, nth) */
    el->state |= AU_STATE_STYLE_DIRTY;
    for (AuNode *s = el->node.next; s; s = s->next)
        if (s->type == AU_NODE_ELEMENT)
            ((AuElement *)s)->state |= AU_STATE_STYLE_DIRTY;
    if (change == AU_CSS_CHANGE_CHILDREN) {
        for (AuNode *c = el->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_ELEMENT)
                ((AuElement *)c)->state |= AU_STATE_STYLE_DIRTY;
    }
    for (AuNode *p = el->node.parent; p && p->type == AU_NODE_ELEMENT; p = p->parent)
        ((AuElement *)p)->state |= AU_STATE_SUBTREE_DIRTY;
}

/* ---- Debug and test access ---- */

size_t css_resolver_entry_count(AuCssResolver *r);
size_t css_resolver_entry_count(AuCssResolver *r)
{
    if (r->index_dirty)
        index_build(r, r->quirks);
    return r->nentries;
}
