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

enum { KEY_ID, KEY_CLASS, KEY_TAG, KEY_ATTR, KEY_UNIVERSAL };

typedef struct {
    uint8_t kind; /* KEY_* */
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
#define RECENT_GROUPS 8
typedef struct {
    const AuStyle *parent;
    AuAtom tag;
    uint8_t ns;
    uint16_t pseudo_bits;
    uint32_t n, npe;
    uint32_t *entries; /* n matched entries, then npe pseudo-element entries */
    uint32_t hash;
    AuStyle *style;
} ShareSlot;

struct AuCssResolver {
    AuCssSheet *ua, *ua_quirks;
    AuCssSheet **sheets;
    uint32_t nsheets, sheets_cap;
    AuCssDevice dev;
    AuCssFontPrefs fprefs;
    AuCssMetricsFn metrics;
    void *metrics_ctx;
    bool index_dirty;
    bool quirks; /* mode the index was built for */
    bool has_has;
    bool styled_once;
    bool full_restyle;
    /* What depends on the viewport. media_deps holds every media query
     * the index evaluated, with its result (the lists belong to the
     * sheets and are valid while the index is). viewport_styles is set
     * once a computed style used a viewport unit. viewport_pending: the
     * viewport changed without flipping a media query, and the styles
     * that use viewport units are not yet computed again. */
    struct MediaDep *media_deps;
    uint32_t nmedia_deps, media_deps_cap;
    bool viewport_styles;
    bool viewport_pending;

    RuleEntry *entries;
    uint32_t nentries, entries_cap;
    uint32_t *sorted; /* entry indices grouped by key */
    AtomMap maps[KEY_UNIVERSAL]; /* by id, class, tag, attribute name */
    uint32_t uni_start, uni_count;
    LayerTree layers[3];

    CssPropertyRule *props; /* @property, borrowed from sheets */
    uint32_t nprops, props_cap;

    Matched *matched;
    uint32_t nmatched, matched_cap;
    uint32_t *matched_entries;
    uint32_t nme, me_cap;
    uint32_t *pseudo_entries; /* matched ::before, ::after and ::marker rules of the element */
    uint32_t npe, pe_cap;
    Winner winners[AU_PROP_LOGICAL_END];
    uint32_t gen;
    uint16_t touched[AU_PROP_LOGICAL_END];
    uint32_t ntouched;

    InlineEntry *inl;
    uint32_t ninl, inl_cap;

    ShareSlot share[SHARE_SLOTS];
    void *recent[G_COUNT][RECENT_GROUPS]; /* referenced groups for intern_groups */
    uint8_t recent_pos[G_COUNT];
    const AuStyle *root_style;
    CssBloom bloom;
    CssHasIndex has_idx; /* built on first use in a style pass when :has() rules exist */
    const AuNode *has_root; /* the document being styled, while has_idx may be built */

    struct StateDep *deps; /* what a state change must restyle besides the element */
    uint32_t ndeps, deps_cap;
    uint32_t state_used; /* state bits some selector reads */
    bool deps_unknown;   /* a selector nested too deeply to analyse: changes restyle everything */
    uint32_t *named;     /* class, id and attribute names selectors use (named_has) */
    uint32_t named_size, named_count;
    struct PendingAttr *pending; /* elements whose class or id changed since the last restyle */
    uint32_t npending, pending_cap;
    uint32_t styled_count; /* elements styled since creation (au_css_resolver_styled_count) */
    uint32_t stat_computed; /* elements computed since the last css_resolver_take_computed */
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

/* The best key of the compound of s that starts at component from: id,
 * else class, else tag, else attribute name; KEY_UNIVERSAL if none. */
static IndexKey compound_key_at(const AuSelector *s, uint32_t from, bool quirks)
{
    IndexKey k = {KEY_UNIVERSAL, 0, 0};
    for (uint32_t i = from; i < s->ncomp && s->comp[i].type != SC_COMBINATOR; i++) {
        const AuSelComp *c = &s->comp[i];
        if (c->type == SC_ID) {
            k.kind = KEY_ID;
            k.atom = quirks ? css_atom_lower(c->name) : c->name;
            return k;
        }
        if (c->type == SC_CLASS && k.kind > KEY_CLASS) {
            k.kind = KEY_CLASS;
            k.atom = quirks ? css_atom_lower(c->name) : c->name;
        } else if (c->type == SC_TAG && k.kind > KEY_TAG) {
            k.kind = KEY_TAG;
            k.atom = c->name;
        } else if (c->type == SC_ATTR && k.kind > KEY_ATTR) {
            k.kind = KEY_ATTR;
            k.atom = c->name;
        }
    }
    return k;
}

static IndexKey compound_key(const AuSelector *s, bool quirks) { return compound_key_at(s, 0, quirks); }

/* Appends the keys under which a selector's subject must be found and
 * returns true, or appends nothing and returns false if it has none. A
 * subject compound without an id, class, tag or attribute of its own is
 * keyed through an :is(), :where() or & whose selectors all have keys
 * (":where(.btn, .link)", "&:hover" nested in ".card"), since the subject
 * must match the subject of one of them. */
static bool subject_keys(const AuSelector *s, bool quirks, uint32_t entry, IndexKey **keys, uint32_t *n,
                         uint32_t *cap, int depth)
{
    IndexKey k = compound_key(s, quirks);
    if (k.kind != KEY_UNIVERSAL) {
        k.entry = entry;
        *CSS_PUSH(*keys, *n, *cap) = k;
        return true;
    }
    if (depth >= 4)
        return false;
    for (uint32_t i = 0; i < s->ncomp && s->comp[i].type != SC_COMBINATOR; i++) {
        const AuSelComp *c = &s->comp[i];
        bool list = (c->type == SC_PSEUDO && (c->op == PC_IS || c->op == PC_WHERE)) || c->type == SC_NESTING;
        if (!list || !c->u.list || !c->u.list->n)
            continue;
        uint32_t mark = *n;
        bool ok = true;
        for (uint32_t j = 0; j < c->u.list->n && ok; j++)
            ok = subject_keys(&c->u.list->sel[j], quirks, entry, keys, n, cap, depth + 1);
        if (ok)
            return true;
        *n = mark;
    }
    return false;
}

static void selector_keys(const AuSelector *s, bool quirks, uint32_t entry, IndexKey **keys, uint32_t *n,
                          uint32_t *cap)
{
    if (!subject_keys(s, quirks, entry, keys, n, cap, 0)) {
        IndexKey k = {KEY_UNIVERSAL, 0, entry};
        *CSS_PUSH(*keys, *n, *cap) = k;
    }
}

/* ---- State and attribute dependencies ----
 *
 * For targeted invalidation of state changes (hover, active, focus,
 * checked...) and of class, id and attribute changes. Every selector of
 * the index is analysed once: for each state pseudo-class it reads and
 * each class, id and attribute it names, where the elements it can style
 * lie relative to the element E that changes.
 *
 * - In the subject compound ("a:hover", "a.on"): E itself. For a state
 *   nothing is recorded, E is always restyled; a class, id or attribute
 *   name goes into the set of names some selector uses (a change of a
 *   name outside it restyles nothing).
 * - Left of a descendant or child combinator (".menu:hover .item"):
 *   descendants of E (REL_DESC).
 * - Left of a sibling combinator ("input:checked + label",
 *   "input:checked ~ div p"): following siblings of E (REL_SIB) or their
 *   subtrees (REL_SIBDESC).
 * - Inside a :has() argument (".card:has(a:hover)"): the :has() anchor is
 *   an ancestor of E, or with a leading + or ~ a preceding sibling of E
 *   or of an ancestor of E. The record keeps the key of the anchor's
 *   compound and where styled elements lie relative to the anchor.
 * - Anywhere the analysis cannot follow (:nth-child(of ...), a complex
 *   selector nested in a :has() argument, :has() in :has()): REL_ANY,
 *   the whole document.
 *
 * A record applies to E only if E has the key (id, class, tag or attribute
 * name) of the compound that reads the state. */
enum { REL_SELF = 0, REL_DESC, REL_SIB, REL_SIBDESC, REL_ANY };
enum { F_STATE = 0, F_CLASS, F_ID, F_ATTR };
#define STATE_OTHER (1u << 31) /* pseudo-classes that follow control values and attributes */
#define STATE_LINK (1u << 30)  /* :link, :any-link (the href attribute) */
#define STATE_LANG (1u << 29)  /* :lang() (lang attributes of the element and its ancestors) */
#define STATE_DIR (1u << 28)   /* :dir() */
#define STATE_ALL (~(uint32_t)(AU_STATE_STYLE_DIRTY | AU_STATE_SUBTREE_DIRTY | AU_STATE_STYLE_SELF_DIRTY))

typedef struct StateDep {
    uint32_t state; /* F_STATE: AU_STATE_* and STATE_* bits; else the class, id or attribute name */
    AuAtom key, anchor;
    uint8_t fkind;    /* F_* */
    uint8_t rel;      /* REL_*: relative to E, or to the anchor when in_has */
    uint8_t in_has;
    uint8_t has_sib;  /* the anchor may be a preceding sibling */
    uint8_t key_kind, anchor_kind; /* KEY_* */
} StateDep;

typedef struct {
    AuCssResolver *r;
    bool in_has, has_sib, has_nested;
    uint8_t has_rel;
    IndexKey anchor;
} DepCtx;

/* The state bits a pseudo-class reads, or 0. */
static uint32_t pseudo_state(int op)
{
    switch (op) {
    case PC_HOVER: return AU_STATE_HOVER;
    case PC_ACTIVE: return AU_STATE_ACTIVE;
    case PC_FOCUS: return AU_STATE_FOCUS;
    case PC_FOCUS_VISIBLE: return AU_STATE_FOCUS_VISIBLE;
    case PC_FOCUS_WITHIN: return AU_STATE_FOCUS_WITHIN | AU_STATE_FOCUS;
    case PC_CHECKED: return AU_STATE_CHECKED | AU_STATE_CHECKED_SET;
    case PC_INDETERMINATE: return AU_STATE_INDETERMINATE | STATE_OTHER;
    case PC_TARGET: return AU_STATE_TARGET;
    case PC_DEFINED: return AU_STATE_DEFINED;
    case PC_VALID:
    case PC_INVALID: return AU_STATE_CHECKED | AU_STATE_CHECKED_SET | STATE_OTHER;
    case PC_DEFAULT:
    case PC_DISABLED:
    case PC_ENABLED:
    case PC_REQUIRED:
    case PC_OPTIONAL:
    case PC_READ_ONLY:
    case PC_READ_WRITE:
    case PC_PLACEHOLDER_SHOWN:
    case PC_USER_VALID:
    case PC_USER_INVALID:
    case PC_IN_RANGE:
    case PC_OUT_OF_RANGE: return STATE_OTHER;
    case PC_LINK:
    case PC_ANY_LINK: return STATE_LINK;
    case PC_LANG: return STATE_LANG;
    case PC_DIR: return STATE_DIR;
    default: return 0;
    }
}

static uint8_t rel_cross(uint8_t rel, int comb)
{
    if (rel == REL_ANY)
        return REL_ANY;
    if (comb == COMB_DESCENDANT || comb == COMB_CHILD)
        return REL_DESC;
    return rel == REL_SELF || rel == REL_SIB ? REL_SIB : REL_SIBDESC;
}

/* The set of class, id and attribute names selectors use: open
 * addressing over (name << 2 | F_*). */
static bool named_has(const AuCssResolver *r, uint8_t fkind, AuAtom a)
{
    if (!r->named_size || !a)
        return false;
    uint32_t key = (a << 2) | fkind;
    for (uint32_t h = (key * 2654435761u) & (r->named_size - 1); r->named[h]; h = (h + 1) & (r->named_size - 1))
        if (r->named[h] == key)
            return true;
    return false;
}

static void named_add(AuCssResolver *r, uint8_t fkind, AuAtom a)
{
    if (!a || named_has(r, fkind, a))
        return;
    if (r->named_count * 2 >= r->named_size) {
        uint32_t size = r->named_size ? r->named_size * 2 : 256;
        uint32_t *t = au_calloc(size * sizeof *t);
        for (uint32_t i = 0; i < r->named_size; i++) {
            if (!r->named[i])
                continue;
            uint32_t h = (r->named[i] * 2654435761u) & (size - 1);
            while (t[h])
                h = (h + 1) & (size - 1);
            t[h] = r->named[i];
        }
        au_free(r->named);
        r->named = t;
        r->named_size = size;
    }
    uint32_t key = (a << 2) | fkind;
    uint32_t h = (key * 2654435761u) & (r->named_size - 1);
    while (r->named[h])
        h = (h + 1) & (r->named_size - 1);
    r->named[h] = key;
    r->named_count++;
}

static void dep_add(DepCtx *cx, uint8_t fkind, uint32_t state, uint8_t rel, IndexKey key)
{
    AuCssResolver *r = cx->r;
    if (fkind == F_STATE) {
        r->state_used |= state;
        /* kept in the subject compound too: only elements with the key
         * of a compound that reads the state are restyled */
    } else {
        /* the element that changes may be gaining or losing the key itself */
        named_add(r, fkind, state);
        key.kind = KEY_UNIVERSAL;
        key.atom = 0;
        if (!cx->in_has && rel == REL_SELF)
            return;
    }
    StateDep d;
    memset(&d, 0, sizeof d);
    d.fkind = fkind;
    d.state = state;
    d.key = key.atom;
    d.key_kind = key.kind;
    d.rel = rel;
    if (cx->in_has) {
        d.in_has = 1;
        d.rel = rel == REL_ANY ? REL_ANY : cx->has_rel;
        d.has_sib = cx->has_sib;
        d.anchor = cx->anchor.atom;
        d.anchor_kind = cx->anchor.kind;
    }
    /* sheets repeat patterns: look at the last few records */
    for (uint32_t i = r->ndeps, n = 0; i-- > 0 && n < 8; n++)
        if (memcmp(&r->deps[i], &d, sizeof d) == 0)
            return;
    *CSS_PUSH(r->deps, r->ndeps, r->deps_cap) = d;
}

/* Analyses selector s, whose subject lies at rel. outer is the key the
 * subject is known to have from an enclosing compound. Recursion follows
 * selector nesting only. */
static void dep_selector(DepCtx *cx, const AuSelector *s, uint8_t rel, IndexKey outer, int depth)
{
    if (depth > 8) {
        cx->r->deps_unknown = true;
        return;
    }
    bool quirks = cx->r->quirks;
    for (uint32_t i = 0; i < s->ncomp;) {
        uint32_t k = i;
        while (k < s->ncomp && s->comp[k].type != SC_COMBINATOR)
            k++;
        IndexKey key = compound_key_at(s, i, quirks);
        if (key.kind == KEY_UNIVERSAL && i == 0)
            key = outer;
        for (uint32_t j = i; j < k; j++) {
            const AuSelComp *c = &s->comp[j];
            const AuSelList *nested = NULL;
            if (c->type == SC_NESTING) {
                nested = c->u.list;
            } else if (c->type == SC_CLASS) {
                dep_add(cx, F_CLASS, quirks ? css_atom_lower(c->name) : c->name, rel, key);
            } else if (c->type == SC_ID) {
                dep_add(cx, F_ID, quirks ? css_atom_lower(c->name) : c->name, rel, key);
            } else if (c->type == SC_ATTR) {
                dep_add(cx, F_ATTR, c->name, rel, key);
            } else if (c->type == SC_PSEUDO) {
                uint32_t st = pseudo_state(c->op);
                if (st) {
                    dep_add(cx, F_STATE, st, rel, key);
                } else if (c->op == PC_IS || c->op == PC_WHERE || c->op == PC_NOT) {
                    nested = c->u.list;
                } else if (c->op == PC_HAS && c->u.list) {
                    DepCtx h = *cx;
                    h.in_has = true;
                    h.has_rel = cx->in_has ? REL_ANY : rel;
                    h.anchor = key;
                    for (uint32_t m = 0; m < c->u.list->n; m++) {
                        const AuSelector *a = &c->u.list->sel[m];
                        h.has_sib = a->rel_comb == COMB_NEXT || a->rel_comb == COMB_SUBSEQUENT;
                        IndexKey none = {KEY_UNIVERSAL, 0, 0};
                        dep_selector(&h, a, cx->in_has ? REL_ANY : REL_SELF, none, depth + 1);
                    }
                } else if (c->op >= PC_NTH_CHILD && c->op <= PC_NTH_LAST_OF_TYPE && c->u.nth.of) {
                    /* a sibling's state changes the index of the others */
                    IndexKey none = {KEY_UNIVERSAL, 0, 0};
                    for (uint32_t m = 0; m < c->u.nth.of->n; m++)
                        dep_selector(cx, &c->u.nth.of->sel[m], REL_ANY, none, depth + 1);
                }
            }
            if (nested) {
                DepCtx n = *cx;
                n.has_nested = cx->in_has;
                for (uint32_t m = 0; m < nested->n; m++)
                    dep_selector(&n, &nested->sel[m], rel, key, depth + 1);
            }
        }
        if (k < s->ncomp)
            rel = cx->has_nested ? REL_ANY : rel_cross(rel, s->comp[k].op);
        i = k + 1;
    }
}

/* By kind, then state or name (ranges are found by binary search). */
static int dep_cmp(const void *a, const void *b)
{
    const StateDep *x = a, *y = b;
    if (x->fkind != y->fkind)
        return x->fkind < y->fkind ? -1 : 1;
    if (x->state != y->state)
        return x->state < y->state ? -1 : 1;
    return memcmp(a, b, sizeof(StateDep));
}

/* The first record of a class, id or attribute name. */
static uint32_t dep_find(const AuCssResolver *r, uint8_t fkind, AuAtom a)
{
    uint32_t lo = 0, hi = r->ndeps;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const StateDep *d = &r->deps[mid];
        if (d->fkind < fkind || (d->fkind == fkind && d->state < a))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void deps_clear(AuCssResolver *r)
{
    r->ndeps = 0;
    r->state_used = 0;
    r->deps_unknown = false;
    au_free(r->named);
    r->named = NULL;
    r->named_size = r->named_count = 0;
}

static void deps_build(AuCssResolver *r)
{
    deps_clear(r);
    DepCtx cx;
    memset(&cx, 0, sizeof cx);
    cx.r = r;
    IndexKey none = {KEY_UNIVERSAL, 0, 0};
    for (uint32_t i = 0; i < r->nentries; i++)
        dep_selector(&cx, r->entries[i].sel, REL_SELF, none, 0);
    if (r->ndeps > 1) {
        qsort(r->deps, r->ndeps, sizeof(StateDep), dep_cmp);
        uint32_t k = 1;
        for (uint32_t i = 1; i < r->ndeps; i++)
            if (memcmp(&r->deps[i], &r->deps[k - 1], sizeof(StateDep)) != 0)
                r->deps[k++] = r->deps[i];
        r->ndeps = k;
    }
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

typedef struct MediaDep {
    const AuCssMediaList *media;
    bool result;
} MediaDep;

/* Evaluates a media query for the index and records it, so that a
 * viewport change can tell whether any result changes. */
static bool media_dep(AuCssResolver *r, const AuCssMediaList *m)
{
    bool ok = css_media_eval(m, &r->dev);
    MediaDep *d = CSS_PUSH(r->media_deps, r->nmedia_deps, r->media_deps_cap);
    d->media = m;
    d->result = ok;
    return ok;
}

static void walk_sheet(AuCssResolver *r, const AuCssSheet *sheet, int32_t layer, uint32_t *order, Frame **stack,
                       uint32_t *sn, uint32_t *scap)
{
    if (sheet->media && !media_dep(r, sheet->media))
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
            descend = media_dep(r, ru->media);
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
            if (!im->sheet || !ru->ok || (ru->media && !media_dep(r, ru->media)))
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
    for (int i = 0; i < KEY_UNIVERSAL; i++)
        map_free(&r->maps[i]);
    for (int i = 0; i < 3; i++)
        r->layers[i].count = 0;
    r->uni_start = r->uni_count = 0;
    r->has_has = false;
    r->nprops = 0;
    r->nmedia_deps = 0;
    deps_clear(r);
}

static void share_clear(AuCssResolver *r);
static void pending_drop(AuCssResolver *r);
static void pending_flush(AuCssResolver *r, AuDocument *doc);

static void sort_registrations(AuCssResolver *r);

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
    if (r->nentries && r->nentries < r->entries_cap) {
        /* Kept until the sheets change: trim to the length. */
        r->entries = au_realloc(r->entries, r->nentries * sizeof *r->entries);
        r->entries_cap = r->nentries;
    }
    sort_registrations(r);
    for (int i = 0; i < 3; i++)
        layer_rank(&r->layers[i]);
    for (uint32_t i = 0; i < r->nentries; i++)
        r->entries[i].layer = r->layers[r->entries[i].origin].n[r->entries[i].layer].rank;
    IndexKey *keys = NULL;
    uint32_t nkeys = 0, kcap = 0;
    for (uint32_t i = 0; i < r->nentries; i++)
        selector_keys(r->entries[i].sel, quirks, i, &keys, &nkeys, &kcap);
    if (nkeys > 1)
        qsort(keys, nkeys, sizeof(IndexKey), key_cmp);
    r->sorted = au_alloc((nkeys ? nkeys : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < nkeys; i++)
        r->sorted[i] = keys[i].entry;
    uint32_t a = 0;
    for (int kind = 0; kind <= KEY_UNIVERSAL; kind++) {
        uint32_t b = a;
        while (b < nkeys && keys[b].kind == kind)
            b++;
        if (kind < KEY_UNIVERSAL)
            map_build(&r->maps[kind], keys, a, b);
        else {
            r->uni_start = a;
            r->uni_count = b - a;
        }
        a = b;
    }
    au_free(keys);
    deps_build(r);
    r->index_dirty = false;
}

/* ---- Resolver lifetime ---- */

/* The user agent sheets are parsed once and shared by every resolver
 * (each document view has one; a parsed copy is about 110 KB). Resolvers
 * only read them. They are freed with the last resolver. */
static AuCssSheet *ua_shared, *ua_quirks_shared;
static uint32_t ua_users;

AuCssResolver *au_css_resolver_new(void)
{
    au_css_init();
    AuCssResolver *r = au_calloc(sizeof *r);
    au_css_device_default(&r->dev);
    au_css_font_prefs_default(&r->fprefs);
    if (!ua_users++) {
        ua_shared = css_sheet_parse(css_ua_sheet, strlen(css_ua_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, false);
        ua_quirks_shared =
            css_sheet_parse(css_ua_quirks_sheet, strlen(css_ua_quirks_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, true);
    }
    r->ua = ua_shared;
    r->ua_quirks = ua_quirks_shared;
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
    for (int g = 0; g < G_COUNT; g++)
        for (int k = 0; k < RECENT_GROUPS; k++)
            if (r->recent[g][k]) {
                css_group_release(g, r->recent[g][k]);
                r->recent[g][k] = NULL;
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
    css_has_index_free(&r->has_idx);
    au_free(r->inl);
    pending_drop(r);
    au_free(r->pending);
    au_free(r->deps);
    au_free(r->entries);
    au_free(r->media_deps);
    for (int i = 0; i < 3; i++)
        au_free(r->layers[i].n);
    au_free(r->props);
    au_free(r->matched);
    au_free(r->matched_entries);
    au_free(r->pseudo_entries);
    if (!--ua_users) {
        au_css_sheet_free(ua_shared);
        au_css_sheet_free(ua_quirks_shared);
        ua_shared = ua_quirks_shared = NULL;
    }
    for (uint32_t i = 0; i < r->nsheets; i++)
        au_css_sheet_free(r->sheets[i]);
    au_free(r->sheets);
    au_free(r);
}

/* The devices differ in nothing but the viewport size. */
static bool device_same_but_viewport(const AuCssDevice *a, const AuCssDevice *b)
{
    return a->device_width == b->device_width && a->device_height == b->device_height &&
           a->resolution == b->resolution && a->print == b->print && a->color_bits == b->color_bits &&
           a->monochrome_bits == b->monochrome_bits && a->dark == b->dark && a->reduced_motion == b->reduced_motion &&
           a->reduced_transparency == b->reduced_transparency && a->contrast_more == b->contrast_more &&
           a->hover == b->hover && a->pointer == b->pointer && a->any_hover == b->any_hover &&
           a->any_pointer == b->any_pointer && a->scripting == b->scripting && a->forced_colors == b->forced_colors &&
           a->grid == b->grid;
}

void au_css_resolver_set_device(AuCssResolver *r, const AuCssDevice *d)
{
    bool tracked = r->styled_once && !r->index_dirty && !r->full_restyle && device_same_but_viewport(&r->dev, d);
    bool same_size = r->dev.width == d->width && r->dev.height == d->height;
    r->dev = *d;
    if (tracked && same_size)
        return;
    if (tracked) {
        /* Only the viewport changed: the index stands unless a media
         * query it evaluated has another result now. */
        bool flipped = false;
        for (uint32_t i = 0; i < r->nmedia_deps && !flipped; i++)
            flipped = css_media_eval(r->media_deps[i].media, &r->dev) != r->media_deps[i].result;
        if (!flipped) {
            r->viewport_pending = r->viewport_pending || r->viewport_styles;
            return;
        }
    }
    r->index_dirty = true;
    r->full_restyle = true;
}

int au_css_resolver_viewport_restyle(const AuCssResolver *r)
{
    if (!r->styled_once || r->index_dirty || r->full_restyle)
        return AU_CSS_VIEWPORT_ALL;
    return r->viewport_pending ? AU_CSS_VIEWPORT_UNITS : AU_CSS_VIEWPORT_NONE;
}

const AuCssDevice *au_css_resolver_device(const AuCssResolver *r) { return &r->dev; }

void au_css_resolver_set_metrics(AuCssResolver *r, AuCssMetricsFn fn, void *ctx)
{
    r->metrics = fn;
    r->metrics_ctx = ctx;
    r->full_restyle = true;
}

void au_css_font_prefs_default(AuCssFontPrefs *p)
{
    p->default_size = 16;
    p->default_mono_size = 13;
    p->text_zoom = 1;
    p->min_size = 0;
}

void au_css_resolver_set_font_prefs(AuCssResolver *r, const AuCssFontPrefs *p)
{
    AuCssFontPrefs d;
    au_css_font_prefs_default(&d);
    /* Written so NaN fails each test. */
    if (p->default_size > 0 && p->default_size < 1000)
        d.default_size = p->default_size;
    if (p->default_mono_size > 0 && p->default_mono_size < 1000)
        d.default_mono_size = p->default_mono_size;
    if (p->text_zoom > 0 && p->text_zoom < 100)
        d.text_zoom = p->text_zoom;
    if (p->min_size >= 0 && p->min_size < 1000)
        d.min_size = p->min_size;
    r->fprefs = d;
    r->full_restyle = true;
}

const AuCssFontPrefs *au_css_resolver_font_prefs(const AuCssResolver *r) { return &r->fprefs; }

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

void au_css_resolver_detach_sheets(AuCssResolver *r)
{
    /* the index points into the sheets */
    index_clear(r);
    share_clear(r);
    r->nsheets = 0;
    au_css_resolver_sheets_changed(r);
}

bool au_css_media_text_matches(const char *query, size_t len, const AuCssDevice *dev)
{
    au_css_init();
    AuCssMediaList *m = css_media_parse_text(query, len);
    bool ok = css_media_eval(m, dev);
    css_media_free(m);
    return ok;
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
            /* The ::before, ::after and ::marker styles hang off the
             * element's style, so the rules that give them are part of
             * what a shared style stands for: find them all. */
            bool attached = e->pseudo == AU_CSS_PSEUDO_BEFORE || e->pseudo == AU_CSS_PSEUDO_AFTER ||
                            e->pseudo == AU_CSS_PSEUDO_MARKER;
            if (!attached && (*pseudo_bits & (1u << e->pseudo)))
                continue; /* already known to match */
            if (css_selector_match(e->sel, el, mc)) {
                *pseudo_bits |= (uint16_t)(1u << e->pseudo);
                if (attached)
                    *CSS_PUSH(r->pseudo_entries, r->npe, r->pe_cap) = ei;
            }
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
    r->npe = 0;
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
    if (map_get(&r->maps[KEY_TAG], tag, &s, &n))
        match_bucket(r, s, n, el, mc, want, pseudo_bits);
    if (r->maps[KEY_ATTR].size)
        for (uint32_t i = 0; i < el->nattrs; i++)
            if (map_get(&r->maps[KEY_ATTR], css_atom_lower(el->attrs[i].name), &s, &n))
                match_bucket(r, s, n, el, mc, want, pseudo_bits);
    match_bucket(r, r->uni_start, r->uni_count, el, mc, want, pseudo_bits);
    if (r->nme > 1) {
        qsort(r->matched_entries, r->nme, sizeof(uint32_t), u32_cmp);
        /* an entry indexed under several keys can be found twice */
        uint32_t k = 1;
        for (uint32_t i = 1; i < r->nme; i++)
            if (r->matched_entries[i] != r->matched_entries[k - 1])
                r->matched_entries[k++] = r->matched_entries[i];
        r->nme = k;
    }
    if (r->npe > 1) {
        qsort(r->pseudo_entries, r->npe, sizeof(uint32_t), u32_cmp);
        uint32_t k = 1;
        for (uint32_t i = 1; i < r->npe; i++)
            if (r->pseudo_entries[i] != r->pseudo_entries[k - 1])
                r->pseudo_entries[k++] = r->pseudo_entries[i];
        r->npe = k;
    }
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
            if (d->prop == AU_PROP_CUSTOM || ((d->flags & DF_QUIRKS_ONLY) && !r->quirks))
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

/* r->props is sorted by name with one entry per name (index_build). */
static const CssPropertyRule *registered(AuCssResolver *r, AuAtom name)
{
    uint32_t lo = 0, hi = r->nprops;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (r->props[mid].name < name)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < r->nprops && r->props[lo].name == name ? &r->props[lo] : NULL;
}

/* True if children of an element with custom properties parent start
 * from exactly parent: every non-inherited registered property has its
 * initial value there and every registered initial value is present. */
static bool parent_map_passes_down(AuCssResolver *r, const AuCustomProps *parent)
{
    for (uint32_t i = 0; i < r->nprops; i++) {
        const CssPropertyRule *p = &r->props[i];
        CssTokList *v = css_custom_get(parent, p->name);
        if (!p->inherits ? v != p->initial : (p->initial && !v))
            return false;
    }
    return true;
}

/* Sorts the @property registrations by name; for a name registered more
 * than once the last registration wins. */
static int prop_rule_cmp(const void *a, const void *b)
{
    const CssPropertyRule *x = *(const CssPropertyRule *const *)a, *y = *(const CssPropertyRule *const *)b;
    if (x->name != y->name)
        return x->name < y->name ? -1 : 1;
    return x < y ? -1 : x > y; /* array order = document order */
}

static void sort_registrations(AuCssResolver *r)
{
    if (r->nprops < 2)
        return;
    const CssPropertyRule **ptrs = au_alloc(r->nprops * sizeof *ptrs);
    for (uint32_t i = 0; i < r->nprops; i++)
        ptrs[i] = &r->props[i];
    qsort(ptrs, r->nprops, sizeof *ptrs, prop_rule_cmp);
    CssPropertyRule *out = au_alloc(r->nprops * sizeof *out);
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->nprops; i++) {
        if (n && out[n - 1].name == ptrs[i]->name)
            out[n - 1] = *ptrs[i];
        else
            out[n++] = *ptrs[i];
    }
    memcpy(r->props, out, n * sizeof *out);
    r->nprops = n;
    au_free(out);
    au_free(ptrs);
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
    bool passes = parent ? parent_map_passes_down(r, parent) : r->nprops == 0;
    if (!nw && passes) {
        au_free(w);
        return css_custom_retain((AuCustomProps *)parent);
    }
    if (nw > 1)
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
    /* Declarations that repeat the parent's values (a universal rule
     * setting the same properties on every element) change nothing. */
    if (passes && parent) {
        bool same = true;
        for (uint32_t i = 0; i < nw && same; i++) {
            const AuCssDecl *d = w[i].d;
            const CssTokList *t = d->u.toks;
            const CssPropertyRule *reg = r->nprops ? registered(r, w[i].name) : NULL;
            same = d->kind == DK_CUSTOM && !t->has_var && !(t->tk.n == 1 && t->tk.t[0].type == TK_IDENT &&
                                                            t->tk.t[0].kw) &&
                   css_custom_get(parent, w[i].name) == t && (!reg || reg->inherits);
        }
        if (same) {
            au_free(w);
            return css_custom_retain((AuCustomProps *)parent);
        }
    }
    /* A few declarations over a large inherited map: keep only this
     * element's entries and refer to the parent's map for the rest. A page
     * with two thousand theme variables and components that each set a
     * few of their own otherwise copies the whole map per component. */
    if (parent && passes && parent->total >= 32 && parent->depth < 12 && nw * 4 <= parent->total) {
        struct CssCustomEntry *own = au_calloc((nw + 1) * sizeof(*own));
        for (uint32_t i = 0; i < nw; i++) {
            const AuCssDecl *d = w[i].d;
            CssTokList *val = NULL;
            if (d->kind == DK_CUSTOM) {
                const CssTokList *t = d->u.toks;
                uint16_t kw = (t->tk.n == 1 && t->tk.t[0].type == TK_IDENT) ? t->tk.t[0].kw : 0;
                if (kw == AU_K_initial)
                    val = NULL;
                else if (kw == AU_K_inherit || kw == AU_K_unset || kw == AU_K_revert || kw == AU_K_revert_layer)
                    val = css_toklist_retain(css_custom_get(parent, w[i].name));
                else
                    val = css_toklist_retain(d->u.toks);
            }
            own[i].name = w[i].name;
            own[i].value = val;
        }
        uint8_t *state = au_calloc(nw + 1);
        VarCtx vc = {r, parent, own, nw, state, 0};
        for (uint32_t i = 0; i < nw; i++)
            resolve_custom(&vc, own[i].name);
        for (uint32_t i = 0; i < nw; i++)
            if (state[i] == 3) {
                css_toklist_release(own[i].value);
                own[i].value = NULL; /* a NULL entry hides the parent's value */
            }
        for (uint32_t i = 0; i < nw && r->nprops; i++)
            if (!own[i].value) {
                const CssPropertyRule *reg = registered(r, own[i].name);
                if (reg && reg->initial)
                    own[i].value = css_toklist_retain(reg->initial);
            }
        au_free(state);
        au_free(w);
        AuCustomProps *c = au_calloc(sizeof *c);
        c->refs = 1;
        c->e = own;
        c->n = nw;
        c->base = css_custom_retain((AuCustomProps *)parent);
        c->depth = (uint16_t)(parent->depth + 1);
        c->total = parent->total + nw;
        return c;
    }
    /* merge with the parent's map into a new sorted map */
    AuCustomProps *c = au_calloc(sizeof *c);
    c->refs = 1;
    struct CssCustomEntry *pe = NULL;
    uint32_t pn = 0;
    bool pe_owned = false;
    if (parent) {
        pe = parent->e;
        pn = parent->n;
        if (parent->base) {
            pe = css_custom_flatten(parent, &pn);
            pe_owned = true;
        }
    }
    c->e = au_calloc((pn + nw + r->nprops + 1) * sizeof(*c->e));
    struct CssCustomEntry *own = au_calloc((nw + 1) * sizeof(*own));
    uint32_t nown = 0;
    uint32_t pi = 0, wi = 0;
    while (pi < pn || wi < nw) {
        bool take_parent = wi >= nw || (pi < pn && pe[pi].name < w[wi].name);
        bool both = pi < pn && wi < nw && pe[pi].name == w[wi].name;
        if (take_parent && !both) {
            const CssPropertyRule *reg = registered(r, pe[pi].name);
            if (!reg || reg->inherits) {
                c->e[c->n].name = pe[pi].name;
                c->e[c->n].value = css_toklist_retain(pe[pi].value);
                c->n++;
            }
            pi++;
            continue;
        }
        const AuCssDecl *d = w[wi].d;
        AuAtom name = w[wi].name;
        CssTokList *pv = both ? pe[pi].value : NULL;
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
    AuCustomProps tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.refs = 1;
    tmp.n = c->n;
    tmp.e = c->e;
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
    c->total = c->n;
    if (pe_owned)
        au_free(pe);
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
static const uint16_t priority_props[] = {AU_PROP_direction, AU_PROP_writing_mode, AU_PROP_font_family, AU_PROP_font_size,
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
    } else if (key > w->key || (key == w->key && d > w->decl)) {
        /* equal keys come from one block: the later declaration wins */
        w->key = key;
        w->decl = d;
    }
}

/* True when el is a form control and the winning declaration of one of
 * its background, border or box-shadow properties has author origin
 * (presentational hints count: they are author level). Browsers then drop
 * the native appearance and paint the CSS box (CSS UI 4, "appearance":
 * devolvable widgets). Runs after logical properties were mapped. */
static bool author_decor(AuCssResolver *r, const AuElement *el)
{
    if (el->ns != AU_NS_HTML)
        return false;
    AuAtom t = el->tag;
    if (t != HN(input) && t != HN(select) && t != HN(textarea) && t != HN(button) && t != HN(progress) &&
        t != HN(meter))
        return false;
    static const uint16_t props[] = {
        AU_PROP_background_color,    AU_PROP_background_image,   AU_PROP_border_top_width,
        AU_PROP_border_right_width,  AU_PROP_border_bottom_width, AU_PROP_border_left_width,
        AU_PROP_border_top_style,    AU_PROP_border_right_style, AU_PROP_border_bottom_style,
        AU_PROP_border_left_style,   AU_PROP_border_top_color,   AU_PROP_border_right_color,
        AU_PROP_border_bottom_color, AU_PROP_border_left_color,  AU_PROP_box_shadow};
    for (size_t i = 0; i < CSS_ARRAY_LEN(props); i++) {
        const Winner *w = &r->winners[props[i]];
        if (w->gen == r->gen && level_origin(w->key) == 2)
            return true;
    }
    return false;
}

static void share_groups(AuStyle *s, const AuStyle *parent, const AuStyle *prev);
static void intern_groups(AuCssResolver *r, AuStyle *s);
static void swap_group(AuStyle *s, int g, void *other);

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
    cs.cc.fp = &r->fprefs;
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
            if ((d->flags & DF_QUIRKS_ONLY) && !r->quirks)
                continue;
            consider(r, d->prop, (d->flags & DF_IMPORTANT) ? mt->key_i : mt->key_n, d);
        }
    }
    /* priority properties */
    for (size_t k = 0; k < CSS_ARRAY_LEN(priority_props); k++) {
        uint16_t p = priority_props[k];
        if (p == AU_PROP_line_height) /* line-height may use the final font size */
            css_compute_font_size_fixup(&cs.cc, r->winners[AU_PROP_font_size].gen == r->gen);
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
    if (!pseudo && el && author_decor(r, el))
        s->flags |= AU_STYLE_F_AUTHOR_DECOR;

    uint16_t pdisp = parent ? parent->box->display : AU_DISPLAY_BLOCK;
    if (pseudo)
        pdisp = AU_DISPLAY_BLOCK; /* pseudo-elements are not flex items of their originating element here */
    css_compute_finish(&cs.cc, pdisp);
    if (cs.cc.is_root)
        s->flags |= AU_STYLE_F_ROOT;
    if (cs.cc.flags & 2) {
        s->flags |= AU_STYLE_F_VIEWPORT_UNITS;
        r->viewport_styles = true;
    }
    if (cs.cc.flags & 4)
        s->flags |= AU_STYLE_F_HAS_ATTR;
    share_groups(s, parent, NULL);
    intern_groups(r, s);
    return s;
}

/* Shares groups equal to one of the last few new groups of their kind
 * (siblings with the same values in a group but different rules). */
static void intern_groups(AuCssResolver *r, AuStyle *s)
{
    for (int g = 0; g < G_COUNT; g++) {
        void *cur = css_style_group(s, g);
        if (*(uint32_t *)cur > 1)
            continue; /* already shared */
        bool found = false;
        for (int k = 0; k < RECENT_GROUPS && !found; k++) {
            void *o = r->recent[g][k];
            if (o && css_group_equal(g, cur, o)) {
                swap_group(s, g, o);
                found = true;
            }
        }
        if (found)
            continue;
        uint8_t pos = r->recent_pos[g];
        if (r->recent[g][pos])
            css_group_release(g, r->recent[g][pos]);
        (*(uint32_t *)cur)++;
        r->recent[g][pos] = cur;
        r->recent_pos[g] = (uint8_t)((pos + 1) % RECENT_GROUPS);
    }
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
    r->stat_computed++;
    r->styled_count++;
    CssMatchCtx mc = {0};
    mc.bloom = bloom;
    mc.quirks = r->quirks;
    mc.has_index = &r->has_idx;
    mc.has_root = r->has_root;
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
        for (uint32_t i = 0; i < r->npe; i++)
            h = (h ^ r->pseudo_entries[i]) * 16777619u;
        slot = &r->share[h % SHARE_SLOTS];
        if (slot->style && slot->hash == h && slot->parent == parent && slot->tag == el->tag && slot->ns == el->ns &&
            slot->pseudo_bits == bits && slot->n == r->nme && slot->npe == r->npe &&
            memcmp(slot->entries, r->matched_entries, r->nme * sizeof(uint32_t)) == 0 &&
            memcmp(slot->entries + r->nme, r->pseudo_entries, r->npe * sizeof(uint32_t)) == 0) {
            css_decl_block_clear(&hints);
            return au_style_retain(slot->style);
        }
    }
    uint32_t npe = r->npe; /* the cascade leaves the match lists alone */
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
        slot->npe = npe;
        slot->entries = au_alloc((r->nme + npe + 1) * sizeof(uint32_t));
        memcpy(slot->entries, r->matched_entries, r->nme * sizeof(uint32_t));
        if (npe)
            memcpy(slot->entries + r->nme, r->pseudo_entries, npe * sizeof(uint32_t));
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
    mc.has_index = &r->has_idx;
    mc.has_root = r->has_root;
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
            if (p->flags & AU_STYLE_F_HAS_ATTR) {
                /* content: attr(x) differs per element: the element's
                 * style, which carries it, must not be shared */
                s->flags |= AU_STYLE_F_HAS_ATTR;
                for (int i = 0; i < SHARE_SLOTS; i++)
                    if (r->share[i].style == s) {
                        au_free(r->share[i].entries);
                        au_style_release(r->share[i].style);
                        memset(&r->share[i], 0, sizeof r->share[i]);
                    }
            }
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

/* Custom property maps with the same names and values. */
static bool custom_same(const AuCustomProps *a, const AuCustomProps *b)
{
    if (a == b)
        return true;
    if (!a || !b || a->n != b->n || a->base != b->base)
        return false;
    for (uint32_t i = 0; i < a->n; i++) {
        const CssTokList *x = a->e[i].value, *y = b->e[i].value;
        if (a->e[i].name != b->e[i].name)
            return false;
        if (x == y)
            continue;
        if (!x || !y || x->tk.n != y->tk.n || x->tk.pool.len != y->tk.pool.len)
            return false;
        if (x->tk.n && memcmp(x->tk.t, y->tk.t, x->tk.n * sizeof(CssToken)) != 0)
            return false;
        if (x->tk.pool.len && memcmp(x->tk.pool.data, y->tk.pool.data, x->tk.pool.len) != 0)
            return false;
    }
    return true;
}

static bool style_same_one(const AuStyle *a, const AuStyle *b)
{
    if (a->flags != b->flags || a->pseudo_bits != b->pseudo_bits || a->pseudo != b->pseudo)
        return false;
    for (int g = 0; g < G_COUNT; g++) {
        const char *x = css_style_group(a, g), *y = css_style_group(b, g);
        if (x == y)
            continue;
        if (!css_group_same(g, x, y))
            return false;
        if (g == G_INH && !custom_same(a->inh->custom, b->inh->custom))
            return false;
    }
    return true;
}

/* True when two computed styles, with their pseudo-element styles, hold
 * the same values. Values kept in separately computed blobs compare as
 * different, which only costs a restyle of the subtree. */
static bool style_same(const AuStyle *a, const AuStyle *b)
{
    if (a == b)
        return true;
    if (!a || !b || !style_same_one(a, b))
        return false;
    const AuStyle *p = a->pseudos, *q = b->pseudos;
    for (; p && q; p = p->next, q = q->next)
        if (p != q && !style_same_one(p, q))
            return false;
    return !p && !q;
}

/* Gives a new style the old style's groups where their values are equal
 * (and likewise for their pseudo-element styles), so that a change of one
 * property leaves the other groups pointer-equal for whoever compares
 * styles by group (layout's style_diff). */
static void adopt_groups(AuStyle *s, const AuStyle *old)
{
    for (; s && old; s = s->pseudo ? s->next : s->pseudos, old = old->pseudo ? old->next : old->pseudos) {
        if (s->pseudo != old->pseudo)
            break;
        for (int g = 0; g < G_COUNT; g++) {
            void *cur = css_style_group(s, g), *og = css_style_group(old, g);
            if (cur == og || !css_group_same(g, cur, og))
                continue;
            if (g == G_INH && !custom_same(s->inh->custom, old->inh->custom))
                continue;
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
            (*(uint32_t *)og)++;
            css_group_release(g, cur);
            *slot = og;
        }
    }
}

/* Styles elements in preorder. With only_dirty, an element is restyled if
 * it or an ancestor is marked AU_STATE_STYLE_DIRTY, or if it is marked
 * AU_STATE_STYLE_SELF_DIRTY; a self-dirty element whose style comes out
 * different has its subtree restyled, since descendants inherit from it.
 * Otherwise every element is restyled. */
static void style_pass(AuCssResolver *r, AuDocument *doc, bool only_dirty)
{
    au_css_init();
    AuElement *de = au_document_element(doc);
    if (!de)
        return;
    prepare(r, de);
    share_clear(r);
    css_bloom_clear(&r->bloom);
    css_has_index_free(&r->has_idx);
    r->has_root = r->has_has ? &doc->node : NULL;
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
        if (forced || (el->state & AU_STATE_STYLE_SELF_DIRTY)) {
            const AuStyle *ps = pe ? au_element_style(pe) : NULL;
            AuStyle *s = compute_element(r, el, ps, &r->bloom);
            if (!pe)
                r->root_style = s; /* its pseudo-elements may use rem */
            attach_pseudos(r, el, s);
            /* Equal values keep the old object, so that what compares
             * style pointers (layout) sees no change. A marked element
             * that changed takes its subtree along: descendants inherit. */
            if (el->style && style_same(s, au_element_style(el))) {
                au_style_release(s);
            } else {
                if (el->style)
                    adopt_groups(s, au_element_style(el));
                store_style(el, s);
                forced = true;
            }
        }
        if (!pe)
            r->root_style = au_element_style(el);
        /* invalidation marks the ancestors of dirty elements subtree-dirty */
        bool sub_dirty = (el->state & AU_STATE_SUBTREE_DIRTY) != 0;
        el->state &= ~(uint32_t)(AU_STATE_STYLE_DIRTY | AU_STATE_SUBTREE_DIRTY | AU_STATE_STYLE_SELF_DIRTY);
        bool descend = n->first_child != NULL && (!only_dirty || forced || sub_dirty);
        if (descend) {
            PathEntry *pe2 = CSS_PUSH(path, np, pcap);
            pe2->el = el;
            pe2->forced = forced;
            css_bloom_push(&r->bloom, el, r->quirks);
            n = n->first_child;
        } else {
            n = au_node_next_skip(n, root);
        }
    }
    au_free(path);
    share_clear(r);
    css_has_index_free(&r->has_idx);
    r->has_root = NULL;
    doc->style_names_n = 0; /* the class and id changes are styled */
    r->styled_once = true;
    r->full_restyle = false;
}

void au_css_style_document(AuCssResolver *r, AuDocument *doc)
{
    r->viewport_pending = false;
    r->viewport_styles = false; /* set again by the styles this pass computes */
    pending_drop(r);
    style_pass(r, doc, false);
}

uint32_t au_css_resolver_styled_count(const AuCssResolver *r) { return r->styled_count; }

static void mark_ancestors(AuElement *el);

/* Marks the elements whose style (or a pseudo-element's) used a viewport
 * unit. Their subtrees follow: a descendant can depend on the value only
 * through inheritance, and the pass goes below an element whose style
 * changed. */
static void mark_viewport_styles(AuDocument *doc)
{
    const AuNode *root = &doc->node;
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (n->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = (AuElement *)n;
        const AuStyle *st = au_element_style(el);
        if (!st)
            continue;
        bool uses = (st->flags & AU_STYLE_F_VIEWPORT_UNITS) != 0;
        for (const AuStyle *ps = st->pseudos; ps && !uses; ps = ps->next)
            uses = (ps->flags & AU_STYLE_F_VIEWPORT_UNITS) != 0;
        if (uses) {
            el->state |= AU_STATE_STYLE_DIRTY;
            mark_ancestors(el);
        }
    }
}

void au_css_restyle(AuCssResolver *r, AuDocument *doc)
{
    if (r->viewport_pending && r->styled_once && !r->full_restyle && !r->index_dirty)
        mark_viewport_styles(doc);
    r->viewport_pending = false;
    if (r->styled_once && !r->full_restyle && !r->index_dirty)
        pending_flush(r, doc);
    pending_drop(r);
    if (!r->styled_once || r->full_restyle || r->index_dirty) {
        r->viewport_styles = false; /* set again by the styles this pass computes */
        style_pass(r, doc, false);
    } else {
        style_pass(r, doc, true);
    }
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

static AuElement *parent_element(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_ELEMENT ? (AuElement *)p : NULL;
}

static void mark_ancestors(AuElement *el)
{
    for (AuElement *p = parent_element(el); p; p = parent_element(p))
        p->state |= AU_STATE_SUBTREE_DIRTY;
}

static bool dep_key_match(const AuCssResolver *r, uint8_t kind, AuAtom atom, const AuElement *el)
{
    bool q = r->quirks;
    switch (kind) {
    case KEY_ID:
        return el->id && (q ? css_atom_lower(el->id) : el->id) == atom;
    case KEY_CLASS:
        for (uint32_t i = 0; i < el->nclasses; i++)
            if ((q ? css_atom_lower(el->classes[i]) : el->classes[i]) == atom)
                return true;
        return false;
    case KEY_TAG:
        return (el->ns == AU_NS_HTML ? el->tag : css_atom_lower(el->tag)) == atom;
    case KEY_ATTR:
        for (uint32_t i = 0; i < el->nattrs; i++)
            if (css_atom_lower(el->attrs[i].name) == atom)
                return true;
        return false;
    default:
        return true;
    }
}

/* Marks what lies at rel from el. */
static void dep_apply(AuCssResolver *r, AuElement *el, uint8_t rel)
{
    switch (rel) {
    case REL_SELF:
        el->state |= AU_STATE_STYLE_SELF_DIRTY;
        mark_ancestors(el);
        break;
    case REL_DESC:
        el->state |= AU_STATE_STYLE_DIRTY;
        mark_ancestors(el);
        break;
    case REL_SIB:
    case REL_SIBDESC:
        for (AuNode *s = el->node.next; s; s = s->next)
            if (s->type == AU_NODE_ELEMENT)
                ((AuElement *)s)->state |= rel == REL_SIB ? AU_STATE_STYLE_SELF_DIRTY : AU_STATE_STYLE_DIRTY;
        mark_ancestors(el);
        break;
    default:
        r->full_restyle = true;
        break;
    }
}

/* Marks what record d reaches from el, whose state or name changed. */
static void dep_fire(AuCssResolver *r, AuElement *el, const StateDep *d)
{
    if (!d->in_has) {
        dep_apply(r, el, d->rel);
        return;
    }
    if (d->rel == REL_ANY) {
        r->full_restyle = true;
        return;
    }
    /* candidate anchors: ancestors of el; with a leading sibling
     * combinator, preceding siblings of el and of its ancestors */
    for (AuElement *a = el; a; a = parent_element(a)) {
        if (d->has_sib) {
            for (AuNode *p = a->node.prev; p; p = p->prev)
                if (p->type == AU_NODE_ELEMENT && dep_key_match(r, d->anchor_kind, d->anchor, (AuElement *)p))
                    dep_apply(r, (AuElement *)p, d->rel);
        } else if (a != el && dep_key_match(r, d->anchor_kind, d->anchor, a)) {
            dep_apply(r, a, d->rel);
        }
    }
}

void au_css_invalidate_state(AuCssResolver *r, AuElement *el, uint32_t bits)
{
    if (!r->styled_once || r->index_dirty || r->full_restyle) {
        r->full_restyle = true; /* the next restyle is a full one, or the records are stale */
        return;
    }
    if (r->deps_unknown) {
        r->full_restyle = true;
        return;
    }
    bits &= STATE_ALL;
    if (!(bits & r->state_used))
        return;
    for (uint32_t i = 0; i < r->ndeps && r->deps[i].fkind == F_STATE && !r->full_restyle; i++) {
        const StateDep *d = &r->deps[i];
        if ((d->state & bits) && dep_key_match(r, d->key_kind, d->key, el))
            dep_fire(r, el, d);
    }
}

/* A class, id or attribute name changed on el: nothing if no selector
 * uses the name; else el, and what the records of the name reach. */
static void invalidate_name(AuCssResolver *r, AuElement *el, uint8_t fkind, AuAtom a)
{
    if (!named_has(r, fkind, a))
        return;
    dep_apply(r, el, REL_SELF);
    for (uint32_t i = dep_find(r, fkind, a); i < r->ndeps && !r->full_restyle; i++) {
        const StateDep *d = &r->deps[i];
        if (d->fkind != fkind || d->state != a)
            break;
        dep_fire(r, el, d);
    }
}

/* What invalidation did before it was targeted, still used for child
 * changes and when the changed names are unknown: the whole document when
 * a :has() rule exists, else the subtrees of el and of its following
 * siblings. */
static void invalidate_coarse(AuCssResolver *r, AuElement *el, bool children)
{
    if (r->has_has) {
        r->full_restyle = true;
        return;
    }
    el->state |= AU_STATE_STYLE_DIRTY;
    for (AuNode *s = el->node.next; s; s = s->next)
        if (s->type == AU_NODE_ELEMENT)
            ((AuElement *)s)->state |= AU_STATE_STYLE_DIRTY;
    if (children) {
        for (AuNode *c = el->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_ELEMENT)
                ((AuElement *)c)->state |= AU_STATE_STYLE_DIRTY;
    }
    mark_ancestors(el);
}

/* Class and id changes are resolved when the restyle starts: the hook that
 * reports them may run before the attribute changes, and the names that
 * changed are in AuDocument.style_names only afterwards. */
typedef struct PendingAttr {
    AuElement *el; /* referenced */
} PendingAttr;

static void pending_drop(AuCssResolver *r)
{
    for (uint32_t i = 0; i < r->npending; i++)
        au_node_unref(&r->pending[i].el->node);
    r->npending = 0;
}

static void pending_flush(AuCssResolver *r, AuDocument *doc)
{
    bool q = r->quirks;
    for (uint32_t i = 0; i < r->npending && !r->full_restyle; i++) {
        AuElement *el = r->pending[i].el;
        if (el->node.doc != doc || !au_node_is_connected(&el->node))
            continue;
        if (doc->style_names_n > AU_DOC_STYLE_NAMES) {
            invalidate_coarse(r, el, false);
            continue;
        }
        for (uint8_t k = 0; k < doc->style_names_n; k++) {
            AuAtom a = q ? css_atom_lower(doc->style_names[k]) : doc->style_names[k];
            invalidate_name(r, el, F_CLASS, a);
            invalidate_name(r, el, F_ID, a);
        }
        invalidate_name(r, el, F_ATTR, AU_ATOM_class);
        invalidate_name(r, el, F_ATTR, AU_ATOM_id);
    }
    pending_drop(r);
}

/* The state bits pseudo-classes read from an attribute, 0 for none. */
static uint32_t attr_state(AuAtom a)
{
    if (a == HN(href))
        return STATE_LINK;
    if (a == HN(checked) || a == HN(selected) || a == HN(type))
        return AU_STATE_CHECKED | AU_STATE_CHECKED_SET | STATE_OTHER;
    if (a == HN(disabled) || a == HN(required) || a == HN(readonly) || a == HN(value) || a == HN(placeholder) ||
        a == HN(min) || a == HN(max) || a == HN(multiple) || a == HN(contenteditable))
        return STATE_OTHER;
    if (a == HN(lang) || a == HN(xml_lang))
        return STATE_LANG;
    if (a == HN(dir_attr))
        return STATE_DIR;
    return 0;
}

/* An attribute presentational hints never read. Anything else restyles
 * the element, which costs one element when nothing comes of it. */
static bool attr_never_hint(const AuElement *el, AuAtom a)
{
    if (el->ns != AU_NS_HTML)
        return false; /* SVG presentation attributes */
    const char *n = au_atom_str(a);
    if (!strncmp(n, "aria-", 5) || !strncmp(n, "data-", 5))
        return true;
    if (a == HN(value))
        return el->tag != HN(li);
    return a == HN(name) || a == HN(placeholder) || a == HN(href) || a == HN(checked) || a == HN(selected) ||
           a == HN(disabled) || a == HN(required) || a == HN(readonly) || a == HN(tabindex) || a == HN(title) ||
           a == HN(min) || a == HN(max) || a == HN(step) || a == HN(multiple) || !strcmp(n, "role") ||
           !strcmp(n, "autocomplete") || !strcmp(n, "for") || !strcmp(n, "target") || !strcmp(n, "rel");
}

static void invalidate_attr(AuCssResolver *r, AuElement *el, AuAtom attr)
{
    if (attr == AU_ATOM_class || attr == AU_ATOM_id) {
        if (r->npending && r->pending[r->npending - 1].el == el)
            return;
        if (r->npending >= 256) {
            r->full_restyle = true;
            pending_drop(r);
            return;
        }
        PendingAttr *p = CSS_PUSH(r->pending, r->npending, r->pending_cap);
        p->el = el;
        au_node_ref(&el->node);
        return;
    }
    AuAtom a = css_atom_lower(attr);
    invalidate_name(r, el, F_ATTR, a);
    uint32_t bits = attr_state(a);
    if (bits & r->state_used) {
        au_css_invalidate_state(r, el, bits);
        /* these reach descendants: :lang() and :dir() of the subtree,
         * :disabled in a fieldset or optgroup, :read-write under contenteditable */
        if (a == HN(lang) || a == HN(xml_lang) || a == HN(dir_attr) || a == HN(contenteditable) ||
            (a == HN(disabled) && (el->tag == HN(fieldset) || el->tag == HN(optgroup))))
            dep_apply(r, el, REL_DESC);
    }
    const AuStyle *st = au_element_style(el);
    bool uses_attr = st && (st->flags & AU_STYLE_F_HAS_ATTR);
    for (const AuStyle *p = st ? st->pseudos : NULL; p && !uses_attr; p = p->next)
        uses_attr = (p->flags & AU_STYLE_F_HAS_ATTR) != 0;
    if (uses_attr || !attr_never_hint(el, a)) {
        /* cells and links take hints from their table and body */
        bool down = el->ns == AU_NS_HTML && (el->tag == HN(table) || el->tag == HN(body));
        dep_apply(r, el, down ? REL_DESC : REL_SELF);
    }
}

void au_css_move_state(AuCssResolver *r, AuElement *from, AuElement *to, uint32_t bit)
{
    /* the nearest common ancestor-or-self keeps the bit */
    AuElement *a = from, *b = to;
    uint32_t da = 0, db = 0;
    for (AuElement *p = a; p; p = parent_element(p))
        da++;
    for (AuElement *p = b; p; p = parent_element(p))
        db++;
    for (; da > db; da--)
        a = parent_element(a);
    for (; db > da; db--)
        b = parent_element(b);
    while (a != b) {
        a = parent_element(a);
        b = parent_element(b);
    }
    AuElement *common = a;
    for (AuElement *p = from; p && p != common; p = parent_element(p))
        if (p->state & bit) {
            p->state &= ~bit;
            if (r)
                au_css_invalidate_state(r, p, bit);
        }
    for (AuElement *p = to; p && p != common; p = parent_element(p))
        if (!(p->state & bit)) {
            p->state |= bit;
            if (r)
                au_css_invalidate_state(r, p, bit);
        }
    /* ancestors that lost the bit while detached get it back */
    for (AuElement *p = common; p && to; p = parent_element(p))
        if (!(p->state & bit)) {
            p->state |= bit;
            if (r)
                au_css_invalidate_state(r, p, bit);
        }
}

uint32_t css_resolver_take_computed(AuCssResolver *r);
uint32_t css_resolver_take_computed(AuCssResolver *r)
{
    uint32_t n = r->stat_computed;
    r->stat_computed = 0;
    return n;
}

bool css_resolver_has_has(AuCssResolver *r);
bool css_resolver_has_has(AuCssResolver *r) { return r->has_has; }

void au_css_invalidate(AuCssResolver *r, AuElement *el, AuCssChange change, AuAtom attr)
{
    if (change == AU_CSS_CHANGE_STATE) {
        au_css_invalidate_state(r, el, ~(uint32_t)0);
        return;
    }
    if (change == AU_CSS_CHANGE_ALL || !r->styled_once || r->index_dirty || r->full_restyle || r->deps_unknown) {
        r->full_restyle = true;
        return;
    }
    switch (change) {
    case AU_CSS_CHANGE_INLINE_STYLE:
        /* the element (its subtree follows if its style changes), and
         * what [style] selectors reach */
        dep_apply(r, el, REL_SELF);
        invalidate_name(r, el, F_ATTR, AU_ATOM_style);
        break;
    case AU_CSS_CHANGE_ATTR:
        if (attr)
            invalidate_attr(r, el, attr);
        else
            invalidate_coarse(r, el, false);
        break;
    default:
        invalidate_coarse(r, el, change == AU_CSS_CHANGE_CHILDREN);
        break;
    }
}

/* ---- Debug and test access ---- */

/* The number of rule entries (one per selector) and of those in the
 * universal bucket, matched against every element. */
void css_resolver_index_stats(AuCssResolver *r, size_t *entries, size_t *universal);
void css_resolver_index_stats(AuCssResolver *r, size_t *entries, size_t *universal)
{
    if (r->index_dirty)
        index_build(r, r->quirks);
    *entries = r->nentries;
    *universal = r->uni_count;
}
