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
 *   key = level(3) context(3) element-attached(1) layer(12) specificity(24)
 *         order(21)
 *   level: 0 UA, 1 user, 2 author; important: 4 author, 5 user, 6 UA.
 *   context: the tree the declaration's sheet belongs to, relative to the
 *   element's own tree (CSS Scoping 3.3, "Shadow Trees and the
 *   Cascade"): for normal declarations an outer tree (a ::part() rule)
 *   wins over the element's tree, which wins over an inner one (:host and
 *   ::slotted() rules); for important declarations the reverse.
 *   layer: rank among the origin's layers (unlayered highest), reversed
 *   for important declarations; presentational hints sit below all
 *   author layers.
 *
 * Shadow trees. Every sheet belongs to a tree scope: the document, or a
 * shadow root (its <style> and <link> elements and its adopted sheets).
 * Shadow roots whose sheet lists are the same share one scope id, and
 * every indexed entry carries its sheet's scope (walked_scope). An
 * element matches the entries of its own tree's scope; a shadow host
 * also matches its shadow scope's entries as the featureless host
 * (:host); a slotted element matches the ::slotted() entries of each
 * slot's scope along its chain of slots; an element with a part
 * attribute matches the ::part() entries of the scopes outside it that
 * its parts reach through exportparts. Until a document has had a shadow
 * root and while no sheet has a scope, none of this runs. */
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

/* A rule entry holds its layer in 16 bits: past MAX_LAYERS layers of an
 * origin, a new layer is its parent. */
#define MAX_LAYERS 65535u

static int32_t layer_new(LayerTree *t, int32_t parent, AuAtom name)
{
    if (t->count >= MAX_LAYERS && parent >= 0)
        return parent;
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

bool css_index_skip_repeats = true;

/* One selector of one rule: 24 bytes with 32-bit pointers. hash is the
 * selector's first ancestor filter hash: match_bucket rejects most
 * entries on it without reading the selector. */
typedef struct RuleEntry {
    const AuSelector *sel;
    /* The rule's declarations (an AuCssDeclBlock *), or, with the low bit
     * set, the rule itself (a CssRule *) whose block is still text:
     * entry_decls parses it and keeps the block here. */
    uintptr_t body;
    uint32_t hash;         /* 0: the selector names no ancestor */
    unsigned order : 24;   /* source order, clamped as make_key clamps it */
    unsigned pseudo : 8;   /* AuCssPseudo of the selector */
    unsigned spec : 24;
    unsigned origin : 8;
    uint16_t layer;        /* node in the origin's tree, then its rank */
    uint16_t sheet;        /* index in AuCssResolver.walked */
} RuleEntry;
#define MAX_WALKED 65535u /* sheets (imports and the user agent's included) one index holds */
CSS_SIZE_ON_32(RuleEntry, 24);

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

#define SCOPE_ANY 0xFFFFu  /* user agent and user sheets: every tree */
#define SCOPE_NONE 0xFFFEu /* a shadow tree without author sheets */
#define SCOPE_MAX 0xFF00u  /* scope ids below this */

typedef struct {
    const AuNode *key; /* a shadow root */
    uint16_t id;
} ScopeKey;

/* Matched entries carry their context in the top bits (ctx_code). */
#define ME_CTX_SHIFT 28
#define ME_INDEX(v) ((v) & ((1u << ME_CTX_SHIFT) - 1))
#define ME_CTX(v) ((int)((v) >> ME_CTX_SHIFT))
#define CTX_OWN 3 /* the element's own tree; 0..2 outer trees, 4..7 inner */

struct AuCssResolver {
    AuCssSheet *ua, *ua_quirks;
    AuCssSheet **sheets;
    const AuNode **sheet_scope; /* per sheet: NULL for the document, else its shadow root */
    uint32_t nsheets, sheets_cap;
    bool any_scoped;  /* some sheet has a shadow root */
    bool scoping;     /* the index was built with scopes: filter by them */
    uint16_t *walked_scope; /* per walked sheet */
    uint32_t walked_scope_cap;
    uint16_t walk_scope;    /* during index_build */
    ScopeKey *scope_keys;   /* shadow root -> scope id, sorted by key */
    uint32_t nscope_keys, scope_keys_cap;
    uint32_t *shadow_entries; /* entries of ::slotted() and ::part() rules (not in the maps) */
    uint32_t nshadow_entries, shadow_entries_cap;
    bool slotted_rules, part_rules;
    const AuNode *cur_root;       /* the tree root of cur_el (style_pass) */
    const AuElement *cur_el;
    AuCssDevice dev;
    AuCssFontPrefs fprefs;
    AuCssMetricsFn metrics;
    void *metrics_ctx;
    bool index_dirty;
    bool quirks; /* mode the index was built for */
    bool has_has;
    bool styled_once;
    bool full_restyle;
    bool sheets_full; /* the sheets changed: the next restyle is a full one (unless sheets_changed_in) */
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
    const AuCssSheet **walked; /* the sheets the index was built from, in order */
    uint32_t nwalked, walked_cap;
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
    /* What child insertions and removals reach (au_css_invalidate_children):
     * for each structural dependency, the set (1 << REL_*) of places the
     * styled elements lie from the element whose match changes. */
    uint8_t st_empty;   /* :empty, on the parent */
    uint8_t st_forward; /* :first-child, :nth-child() and the of-type forms, on following siblings */
    uint8_t st_back;    /* :last-child, :nth-last-child() and the of-type forms, on preceding siblings */
    uint8_t st_adj;     /* E + F: F, the element after the change */
    uint8_t st_ind;     /* E ~ F: every following sibling */
    bool st_has_empty;  /* :empty inside :has(): a text change reaches the :has() anchors */
    bool st_form;       /* :default or :indeterminate: one control's match follows the others of its form */
    struct HasAnchor *has_anchors; /* every :has() (au_css_invalidate_children) */
    uint32_t nhas_anchors, has_anchors_cap;
    uint32_t styled_count; /* elements styled since creation (au_css_resolver_styled_count) */
    uint32_t stat_computed; /* elements computed since the last css_resolver_take_computed */

    /* Container queries. cq_conds[1..ncq) are the @container rules the
     * index walked, each with the one around it (0 for none); entry_cq
     * gives each entry's innermost one, and is NULL while no entry is in
     * an @container rule, so a sheet without them matches as before.
     * cq_recs is a hash table of the containers whose size a query or a
     * container unit read, with that size (each holds a reference to its
     * element). cq_pend: an element whose new style is not stored yet
     * while its pseudo-elements are computed. */
    struct CqCond *cq_conds;
    uint32_t ncq, cq_cap;
    uint16_t *entry_cq;
    uint32_t entry_cq_cap;
    struct CqRec *cq_recs;
    uint32_t ncq_recs, cq_recs_size; /* size: a power of two, or 0 */
    AuCssContainerSizeFn cq_size;
    void *cq_size_ctx;
    const AuElement *cq_pend_el;
    const AuStyle *cq_pend_style;
    uint32_t cq_gen; /* memos of cq_conds older than this are stale */
    bool cq_pseudo;  /* collect found a pseudo-element rule inside @container */
    struct CqNear *cq_near; /* CQ_NEAR_SIZE slots, or NULL */
};

/* The nearest container (ancestor or self of el) with a name and axes,
 * found in one style pass. Without it a deep document would walk to the
 * root from every element. */
#define CQ_NEAR_SIZE 256
typedef struct CqNear {
    const AuElement *el, *res;
    AuAtom name;
    uint32_t gen;
    unsigned mask;
} CqNear;

typedef struct CqCond {
    const CssContainerQuery *q; /* the sheet's, valid while the index is */
    uint32_t parent;
    /* the last result, for the elements whose search starts at memo_el
     * (siblings share one), within one style pass */
    const AuElement *memo_el;
    uint32_t memo_gen;
    bool memo;
} CqCond;

typedef struct CqRec {
    AuElement *el; /* NULL: a free slot */
    float w, h;
} CqRec;

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
            k.atom = quirks ? css_atom_lower(c->u.name) : c->u.name;
            return k;
        }
        if (c->type == SC_CLASS && k.kind > KEY_CLASS) {
            k.kind = KEY_CLASS;
            k.atom = quirks ? css_atom_lower(c->u.name) : c->u.name;
        } else if (c->type == SC_TAG && k.kind > KEY_TAG) {
            k.kind = KEY_TAG;
            k.atom = css_comp_name(c);
        } else if (c->type == SC_ATTR && k.kind > KEY_ATTR) {
            k.kind = KEY_ATTR;
            k.atom = css_comp_name(c);
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

/* A :has() pseudo-class: the elements it can match (anchor) and where the
 * elements whose style follows its result lie from it (rel). sib: an
 * argument starts with + or ~, so the anchor may be a preceding sibling of
 * what changed. */
typedef struct HasAnchor {
    AuAtom anchor;
    uint8_t anchor_kind; /* KEY_* */
    uint8_t rel;
    uint8_t sib;
} HasAnchor;

static void has_anchor_add(AuCssResolver *r, IndexKey anchor, uint8_t rel, bool sib)
{
    HasAnchor h;
    memset(&h, 0, sizeof h);
    h.anchor = anchor.atom;
    h.anchor_kind = anchor.kind;
    h.rel = rel;
    h.sib = sib;
    for (uint32_t i = 0; i < r->nhas_anchors; i++)
        if (!memcmp(&r->has_anchors[i], &h, sizeof h))
            return;
    *CSS_PUSH(r->has_anchors, r->nhas_anchors, r->has_anchors_cap) = h;
}

/* The structural pseudo-class op, read at rel, outside :has(). */
static void struct_pseudo(AuCssResolver *r, int op, uint8_t rel)
{
    uint8_t bit = (uint8_t)(1u << rel);
    if (op == PC_DEFAULT || op == PC_INDETERMINATE)
        r->st_form = true;
    switch (op) {
    case PC_EMPTY: r->st_empty |= bit; break;
    case PC_FIRST_CHILD:
    case PC_NTH_CHILD:
    case PC_FIRST_OF_TYPE:
    case PC_NTH_OF_TYPE: r->st_forward |= bit; break;
    case PC_LAST_CHILD:
    case PC_NTH_LAST_CHILD:
    case PC_LAST_OF_TYPE:
    case PC_NTH_LAST_OF_TYPE: r->st_back |= bit; break;
    case PC_ONLY_CHILD:
    case PC_ONLY_OF_TYPE:
        r->st_forward |= bit;
        r->st_back |= bit;
        break;
    default: break;
    }
}

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
    case PC_MODAL: return AU_STATE_MODAL;
    case PC_POPOVER_OPEN: return AU_STATE_POPOVER_OPEN;
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
                dep_add(cx, F_CLASS, quirks ? css_atom_lower(c->u.name) : c->u.name, rel, key);
            } else if (c->type == SC_ID) {
                dep_add(cx, F_ID, quirks ? css_atom_lower(c->u.name) : c->u.name, rel, key);
            } else if (c->type == SC_ATTR) {
                dep_add(cx, F_ATTR, css_comp_name(c), rel, key);
            } else if (c->type == SC_PSEUDO) {
                uint32_t st = pseudo_state(c->op);
                IndexKey none = {KEY_UNIVERSAL, 0, 0};
                if (!cx->in_has)
                    struct_pseudo(cx->r, c->op, rel);
                else if (c->op == PC_EMPTY)
                    cx->r->st_has_empty = true;
                else if (c->op == PC_DEFAULT || c->op == PC_INDETERMINATE)
                    cx->r->st_form = true;
                if (st) {
                    /* a state after ::part() is the part element's, which
                     * has none of the host compound's keys */
                    dep_add(cx, F_STATE, st, rel, (c->flags & SCF_ON_PART) ? none : key);
                } else if (c->op == PC_HOST || c->op == PC_SLOTTED) {
                    /* :host(): the host, whose flat subtree the shadow
                     * tree is; ::slotted(): the slotted element, which has
                     * none of the slot compound's keys */
                    if (c->u.list)
                        for (uint32_t m = 0; m < c->u.list->n; m++)
                            dep_selector(cx, &c->u.list->sel[m], rel, none, depth + 1);
                } else if (c->op == PC_HOST_CONTEXT && c->u.list) {
                    /* an ancestor of the host: its subtree holds the host */
                    for (uint32_t m = 0; m < c->u.list->n; m++)
                        dep_selector(cx, &c->u.list->sel[m], cx->in_has ? REL_ANY : REL_DESC, none, depth + 1);
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
                        has_anchor_add(cx->r, key, h.has_rel, h.has_sib);
                        IndexKey none = {KEY_UNIVERSAL, 0, 0};
                        dep_selector(&h, a, cx->in_has ? REL_ANY : REL_SELF, none, depth + 1);
                    }
                } else if (c->op >= PC_NTH_CHILD && c->op <= PC_NTH_LAST_OF_TYPE && c->u.ext->u.nth.of) {
                    /* a sibling's state changes the index of the others */
                    IndexKey none = {KEY_UNIVERSAL, 0, 0};
                    for (uint32_t m = 0; m < c->u.ext->u.nth.of->n; m++)
                        dep_selector(cx, &c->u.ext->u.nth.of->sel[m], REL_ANY, none, depth + 1);
                }
            }
            if (nested) {
                DepCtx n = *cx;
                n.has_nested = cx->in_has;
                for (uint32_t m = 0; m < nested->n; m++)
                    dep_selector(&n, &nested->sel[m], rel, key, depth + 1);
            }
        }
        if (k < s->ncomp) {
            /* Components are stored subject first: this compound is right
             * of the combinator. An element inserted or removed before
             * the one it matches changes what it is next to. */
            int op = s->comp[k].op;
            if (!cx->in_has && op == COMB_NEXT)
                cx->r->st_adj |= (uint8_t)(1u << rel);
            else if (!cx->in_has && op == COMB_SUBSEQUENT)
                cx->r->st_ind |= (uint8_t)(1u << rel);
            rel = cx->has_nested ? REL_ANY : rel_cross(rel, op);
        }
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
    r->st_empty = r->st_forward = r->st_back = r->st_adj = r->st_ind = 0;
    r->st_has_empty = r->st_form = false;
    r->nhas_anchors = 0;
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
    for (uint32_t i = 0; i < r->nentries; i++) {
        /* a ::slotted() or ::part() rule styles elements below the slot
         * or host its compound matches (in the flat tree) */
        uint8_t pe = r->entries[i].pseudo;
        bool below = pe == AU_CSS_PSEUDO_SLOTTED || pe == AU_CSS_PSEUDO_PART;
        dep_selector(&cx, r->entries[i].sel, below ? REL_DESC : REL_SELF, none, 0);
    }
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
    uint32_t sheet_index;
    int32_t layer;
    uint32_t cq; /* the innermost @container rule around it, 0 for none */
} Frame;

/* Keeps entry_cq as long as the entries array. */
static void entry_cq_sync(AuCssResolver *r)
{
    if (r->entry_cq_cap >= r->entries_cap)
        return;
    r->entry_cq = au_realloc(r->entry_cq, r->entries_cap * sizeof *r->entry_cq);
    memset(r->entry_cq + r->entry_cq_cap, 0, (r->entries_cap - r->entry_cq_cap) * sizeof *r->entry_cq);
    r->entry_cq_cap = r->entries_cap;
}

static void add_entries(AuCssResolver *r, const AuSelList *sel, const CssRule *rule, uint32_t sheet_index,
                        int32_t layer, uint32_t *order, bool nested_decls, uint32_t cq)
{
    uint32_t first = r->nentries;
    uint8_t origin = r->walked[sheet_index]->origin;
    /* a block that is still text has at least one declaration to try */
    if (!sel || (!(rule->flags & RF_LAZY) && !rule->b.decls->n))
        return;
    if (r->nentries + sel->n >= (1u << ME_CTX_SHIFT))
        return; /* matched entries keep their context above the index */
    uint32_t spec_override = nested_decls ? css_sellist_max_spec(sel) : 0;
    uint32_t ord = (*order)++;
    for (uint32_t i = 0; i < sel->n; i++) {
        if (sel->sel[i].flags & SELF_NEVER)
            continue; /* cannot match: not indexed, and its block is never parsed */
        RuleEntry *e = CSS_PUSH(r->entries, r->nentries, r->entries_cap);
        e->sel = &sel->sel[i];
        e->body = rule->flags & RF_LAZY ? (uintptr_t)rule | 1 : (uintptr_t)rule->b.decls;
        e->hash = sel->sel[i].nhash ? css_sel_hashes(&sel->sel[i])[0] : 0;
        e->sheet = (uint16_t)sheet_index;
        e->order = ord > 0xFFFFFF ? 0xFFFFFF : ord;
        e->spec = nested_decls ? spec_override : sel->sel[i].spec;
        e->layer = (uint16_t)layer;
        e->origin = origin;
        e->pseudo = sel->sel[i].pseudo;
        if (sel->sel[i].flags & SELF_HAS)
            r->has_has = true;
    }
    if (cq || r->entry_cq) {
        entry_cq_sync(r);
        for (uint32_t i = first; i < r->nentries; i++)
            r->entry_cq[i] = (uint16_t)cq;
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

/* Adds a sheet's rules to the index. With layers_only, only names its
 * layers in the order a full walk would (an occurrence of a sheet that
 * is given again later: index_build). */
static void walk_sheet(AuCssResolver *r, const AuCssSheet *sheet, int32_t layer, uint32_t *order, Frame **stack,
                       uint32_t *sn, uint32_t *scap, bool layers_only)
{
    if (sheet->media && !media_dep(r, sheet->media))
        return;
    if (r->nwalked >= MAX_WALKED)
        return;
    uint32_t sheet_index = r->nwalked;
    *CSS_PUSH(r->walked, r->nwalked, r->walked_cap) = sheet;
    if (r->walked_scope_cap < r->walked_cap) {
        r->walked_scope_cap = r->walked_cap;
        r->walked_scope = au_realloc(r->walked_scope, r->walked_scope_cap * sizeof *r->walked_scope);
    }
    r->walked_scope[sheet_index] = sheet->origin == AU_CSS_ORIGIN_AUTHOR ? r->walk_scope : SCOPE_ANY;
    LayerTree *lt = &r->layers[sheet->origin];
    for (uint32_t i = 0; i < sheet->nprops && !layers_only; i++)
        *CSS_PUSH(r->props, r->nprops, r->props_cap) = sheet->props[i];
    /* frames are processed in order: push the first rule */
    uint32_t base = *sn;
    if (sheet->rules) {
        Frame *f = CSS_PUSH(*stack, *sn, *scap);
        f->rule = sheet->rules;
        f->sheet = sheet;
        f->sheet_index = sheet_index;
        f->layer = layer;
        f->cq = 0;
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
            nf->sheet_index = f.sheet_index;
            nf->layer = f.layer;
            nf->cq = f.cq;
        }
        int32_t child_layer = f.layer;
        uint32_t child_cq = f.cq;
        bool descend = false;
        switch (ru->type) {
        case RULE_STYLE:
            if (!layers_only)
                add_entries(r, ru->u.sel, ru, f.sheet_index, f.layer, order, false, f.cq);
            descend = true;
            break;
        case RULE_NESTED_DECLS:
            if (!layers_only)
                add_entries(r, ru->u.sel, ru, f.sheet_index, f.layer, order, true, f.cq);
            break;
        case RULE_CONTAINER:
            /* evaluated per element (cq_match); past 65535 rules the rest
             * are left out rather than applied without their condition */
            if (r->ncq < 0xFFFFu) {
                if (!r->ncq) /* index 0 stands for none */
                    memset(CSS_PUSH(r->cq_conds, r->ncq, r->cq_cap), 0, sizeof(CqCond));
                CqCond *cd = CSS_PUSH(r->cq_conds, r->ncq, r->cq_cap);
                memset(cd, 0, sizeof *cd);
                cd->q = ru->u.cq;
                cd->parent = f.cq;
                child_cq = r->ncq - 1;
                descend = true;
            }
            break;
        case RULE_MEDIA:
            descend = media_dep(r, ru->u.media);
            break;
        case RULE_SUPPORTS:
            descend = (ru->flags & RF_OK) != 0;
            break;
        case RULE_LAYER_BLOCK:
            child_layer = layer_path(lt, f.layer, &ru->u.layers[0]);
            descend = true;
            break;
        case RULE_LAYER_STMT:
            for (uint32_t k = 0; k < ru->b.nlayers; k++)
                layer_path(lt, f.layer, &ru->u.layers[k]);
            break;
        case RULE_IMPORT: {
            const AuCssImport *im = &f.sheet->imports[ru->b.index];
            if (!im->sheet || !(ru->flags & RF_OK) || (ru->u.media && !media_dep(r, ru->u.media)))
                break;
            int32_t il = f.layer;
            if (ru->flags & RF_LAYER)
                il = layer_path(lt, f.layer, &f.sheet->import_layers[ru->b.index]);
            /* the imported sheet's rules come here in order: walk it now */
            walk_sheet(r, im->sheet, il, order, stack, sn, scap, layers_only);
            break;
        }
        default:
            break; /* @scope, @starting-style: not applied */
        }
        const CssRule *kids = descend ? css_rule_children(f.sheet, ru) : NULL;
        if (kids) {
            Frame *nf = CSS_PUSH(*stack, *sn, *scap);
            nf->rule = kids;
            nf->sheet = f.sheet;
            nf->sheet_index = f.sheet_index;
            nf->layer = child_layer;
            nf->cq = child_cq;
        }
    }
}

static void index_clear(AuCssResolver *r)
{
    r->nentries = 0;
    r->nwalked = 0;
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
    r->nscope_keys = 0;
    r->nshadow_entries = 0;
    r->slotted_rules = r->part_rules = false;
    r->ncq = 0;
    au_free(r->entry_cq);
    r->entry_cq = NULL;
    r->entry_cq_cap = 0;
    deps_clear(r);
}

static void share_clear(AuCssResolver *r);
static void pending_drop(AuCssResolver *r);
static void pending_flush(AuCssResolver *r, AuDocument *doc);

static void sort_registrations(AuCssResolver *r);

/* Walks the sheets of one tree scope, list[0..n) in order, under layer
 * node layer of the author tree (0 for the document's). */
static void walk_group(AuCssResolver *r, AuCssSheet *const *list, uint32_t n, int32_t layer, uint32_t *order,
                       Frame **stack, uint32_t *sn, uint32_t *scap)
{
    for (uint32_t i = 0; i < n; i++) {
        /* A sheet given again later (a page that links one sheet several
         * times): every declaration of this occurrence loses to the same
         * declaration of the later one, which has the same origin, layer
         * and specificity and comes later, so only the last occurrence is
         * indexed. An earlier occurrence still names its layers, whose
         * place is set by where they are first named. An anonymous layer
         * is a new layer at each occurrence, so a sheet with one is
         * indexed every time (repeat_matters). */
        bool again = false;
        if (css_index_skip_repeats && !list[i]->repeat_matters)
            for (uint32_t k = i + 1; k < n && !again; k++)
                again = list[k] == list[i];
        if (!again || list[i]->has_layers)
            walk_sheet(r, list[i], list[i]->origin == AU_CSS_ORIGIN_AUTHOR ? layer : 0, order, stack, sn, scap,
                       again);
    }
}

typedef struct {
    const AuNode *key;
    uint32_t index;
} ScopedSheet;

static int scoped_cmp(const void *a, const void *b)
{
    const ScopedSheet *x = a, *y = b;
    if (x->key != y->key)
        return (uintptr_t)x->key < (uintptr_t)y->key ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* The shadow roots' sheets: one scope id for each distinct list of
 * sheets, each walked once under a layer node of its own (a scope's
 * layers are ordered among themselves only). */
static void index_scopes(AuCssResolver *r, uint32_t *order, Frame **stack, uint32_t *sn, uint32_t *scap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->nsheets; i++)
        n += r->sheet_scope[i] != NULL;
    if (!n)
        return;
    ScopedSheet *v = au_alloc(n * sizeof *v);
    n = 0;
    for (uint32_t i = 0; i < r->nsheets; i++)
        if (r->sheet_scope[i]) {
            v[n].key = r->sheet_scope[i];
            v[n].index = i;
            n++;
        }
    qsort(v, n, sizeof *v, scoped_cmp);
    /* groups already walked: [first, first + len) in v, and their ids */
    typedef struct {
        uint32_t first, len, hash;
        uint16_t id;
    } Group;
    Group *groups = NULL;
    uint32_t ngroups = 0, gcap = 0;
    AuCssSheet **list = au_alloc(n * sizeof *list);
    for (uint32_t a = 0; a < n;) {
        uint32_t b = a;
        uint32_t h = 2166136261u;
        while (b < n && v[b].key == v[a].key) {
            h = (h ^ (uint32_t)(uintptr_t)r->sheets[v[b].index]) * 16777619u;
            b++;
        }
        uint16_t id = 0;
        for (uint32_t g = 0; g < ngroups && !id; g++) {
            if (groups[g].hash != h || groups[g].len != b - a)
                continue;
            bool same = true;
            for (uint32_t k = 0; k < b - a && same; k++)
                same = r->sheets[v[groups[g].first + k].index] == r->sheets[v[a + k].index];
            if (same)
                id = groups[g].id;
        }
        if (!id && ngroups + 1 < SCOPE_MAX) {
            id = (uint16_t)(ngroups + 1);
            Group *gr = CSS_PUSH(groups, ngroups, gcap);
            gr->first = a;
            gr->len = b - a;
            gr->hash = h;
            gr->id = id;
            for (uint32_t k = a; k < b; k++)
                list[k - a] = r->sheets[v[k].index];
            r->walk_scope = id;
            int32_t layer = layer_new(&r->layers[AU_CSS_ORIGIN_AUTHOR], 0, 0);
            walk_group(r, list, b - a, layer, order, stack, sn, scap);
            r->walk_scope = 0;
        }
        if (id) {
            ScopeKey *sk = CSS_PUSH(r->scope_keys, r->nscope_keys, r->scope_keys_cap);
            sk->key = v[a].key;
            sk->id = id;
        }
        a = b;
    }
    /* v was sorted by key: so is scope_keys (scope_of_root searches it) */
    au_free(list);
    au_free(groups);
    au_free(v);
}

/* The scope id of a tree root: 0 for the document (and for detached
 * trees), the id of a shadow root's sheet list, or SCOPE_NONE. */
static uint16_t scope_of_root(const AuCssResolver *r, const AuNode *root)
{
    if (!root || !au_is_shadow_root(root))
        return 0;
    uint32_t lo = 0, hi = r->nscope_keys;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const AuNode *k = r->scope_keys[mid].key;
        if (k == root)
            return r->scope_keys[mid].id;
        if ((uintptr_t)k < (uintptr_t)root)
            lo = mid + 1;
        else
            hi = mid;
    }
    return SCOPE_NONE;
}

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
    r->walk_scope = 0;
    walk_sheet(r, r->ua, 0, &order, &stack, &sn, &scap, false);
    if (quirks)
        walk_sheet(r, r->ua_quirks, 0, &order, &stack, &sn, &scap, false);
    if (!r->any_scoped) {
        walk_group(r, r->sheets, r->nsheets, 0, &order, &stack, &sn, &scap);
    } else {
        /* the document's sheets in order, then each shadow scope's */
        AuCssSheet **list = au_alloc((r->nsheets ? r->nsheets : 1) * sizeof *list);
        uint32_t n = 0;
        for (uint32_t i = 0; i < r->nsheets; i++)
            if (!r->sheet_scope[i])
                list[n++] = r->sheets[i];
        walk_group(r, list, n, 0, &order, &stack, &sn, &scap);
        au_free(list);
        index_scopes(r, &order, &stack, &sn, &scap);
    }
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
    for (uint32_t i = 0; i < r->nentries; i++) {
        uint8_t pe = r->entries[i].pseudo;
        if (pe == AU_CSS_PSEUDO_SLOTTED || pe == AU_CSS_PSEUDO_PART) {
            /* matched for the slotted or part element, against another
             * element (the slot, the host): not found by the keys */
            *CSS_PUSH(r->shadow_entries, r->nshadow_entries, r->shadow_entries_cap) = i;
            if (pe == AU_CSS_PSEUDO_SLOTTED)
                r->slotted_rules = true;
            else
                r->part_rules = true;
            continue;
        }
        selector_keys(r->entries[i].sel, quirks, i, &keys, &nkeys, &kcap);
    }
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
        /* every document's resolver shares them: their atoms live for good */
        AuAtomBits prev = au_atom_scope_enter(0);
        ua_shared = css_sheet_parse(css_ua_sheet, strlen(css_ua_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, false, CSS_TEXT_BORROW);
        ua_quirks_shared =
            css_sheet_parse(css_ua_quirks_sheet, strlen(css_ua_quirks_sheet), AU_CSS_ORIGIN_UA, NULL, NULL, true,
                            CSS_TEXT_BORROW);
        au_atom_scope_leave(prev);
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
    au_free(r->has_anchors);
    au_free(r->entries);
    au_free(r->walked);
    au_free(r->walked_scope);
    au_free(r->scope_keys);
    au_free(r->shadow_entries);
    au_free(r->sheet_scope);
    au_free(r->media_deps);
    for (int i = 0; i < 3; i++)
        au_free(r->layers[i].n);
    au_free(r->props);
    au_free(r->matched);
    au_free(r->matched_entries);
    au_free(r->pseudo_entries);
    au_css_resolver_forget_containers(r);
    au_free(r->cq_recs);
    au_free(r->cq_conds);
    au_free(r->cq_near);
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

void au_css_resolver_add_scoped_sheet(AuCssResolver *r, AuCssSheet *s, const AuNode *scope)
{
    if (scope && !au_is_shadow_root(scope))
        scope = NULL;
    uint32_t cap = r->sheets_cap;
    *CSS_PUSH(r->sheets, r->nsheets, r->sheets_cap) = s;
    if (r->sheets_cap != cap || !r->sheet_scope)
        r->sheet_scope = au_realloc(r->sheet_scope, r->sheets_cap * sizeof *r->sheet_scope);
    r->sheet_scope[r->nsheets - 1] = scope;
    if (scope)
        r->any_scoped = true;
    au_css_resolver_sheets_changed(r);
}

void au_css_resolver_add_sheet(AuCssResolver *r, AuCssSheet *s)
{
    au_css_resolver_add_scoped_sheet(r, s, NULL);
}

void au_css_resolver_remove_sheet(AuCssResolver *r, AuCssSheet *s)
{
    for (uint32_t i = 0; i < r->nsheets; i++)
        if (r->sheets[i] == s) {
            memmove(&r->sheets[i], &r->sheets[i + 1], (r->nsheets - i - 1) * sizeof(AuCssSheet *));
            memmove(&r->sheet_scope[i], &r->sheet_scope[i + 1], (r->nsheets - i - 1) * sizeof *r->sheet_scope);
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
    r->any_scoped = false;
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
    r->sheets_full = true;
}

static void mark_ancestors(AuElement *el);

void au_css_resolver_sheets_changed_in(AuCssResolver *r, AuNode *const *roots, size_t n)
{
    r->index_dirty = true;
    if (!r->styled_once || r->full_restyle)
        return;
    for (size_t i = 0; i < n; i++)
        if (!roots[i] || !au_is_shadow_root(roots[i]) || !((const AuShadowRoot *)roots[i])->host)
            return; /* the document's sheets: restyle everything */
    r->sheets_full = false;
    /* the rules of these trees reach their shadow trees, their hosts and
     * what is slotted into them: the hosts' flat subtrees */
    for (size_t i = 0; i < n; i++) {
        AuElement *h = ((const AuShadowRoot *)roots[i])->host;
        h->state |= AU_STATE_STYLE_DIRTY;
        mark_ancestors(h);
    }
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

/* ---- Container queries ----
 * The query container of an element is its nearest flat-tree ancestor
 * (for a pseudo-element, its element first) whose container-name holds
 * the query's name, if it gives one, and whose container-type allows
 * querying every axis the condition reads (CSS Conditional 5, "Container
 * Queries"). Its size comes from the embedder (the last layout) and is
 * recorded in cq_recs, so that au_css_resolver_containers_changed can
 * tell after the next layout which containers' descendants were styled
 * with another size. */

static AuElement *flat_parent_element(const AuElement *el);

void au_css_resolver_set_container_size(AuCssResolver *r, AuCssContainerSizeFn fn, void *ctx)
{
    r->cq_size = fn;
    r->cq_size_ctx = ctx;
}

bool au_css_resolver_has_containers(const AuCssResolver *r) { return r->ncq_recs != 0; }

static uint32_t cq_hash(const AuElement *el, uint32_t size)
{
    uintptr_t p = (uintptr_t)el;
    return (uint32_t)((p >> 3) * 2654435761u) & (size - 1);
}

static CqRec *cq_rec_find(AuCssResolver *r, const AuElement *el)
{
    if (!r->cq_recs_size)
        return NULL;
    for (uint32_t i = cq_hash(el, r->cq_recs_size), k = 0; k < r->cq_recs_size; k++, i = (i + 1) & (r->cq_recs_size - 1)) {
        if (r->cq_recs[i].el == el)
            return &r->cq_recs[i];
        if (!r->cq_recs[i].el)
            return NULL;
    }
    return NULL;
}

/* Puts an entry in a table of size slots (more than its entries). */
static CqRec *cq_rec_slot(CqRec *t, uint32_t size, const AuElement *el)
{
    uint32_t i = cq_hash(el, size);
    while (t[i].el && t[i].el != el)
        i = (i + 1) & (size - 1);
    return &t[i];
}

static void cq_rec_rehash(AuCssResolver *r, uint32_t size)
{
    CqRec *t = au_calloc(size * sizeof *t);
    for (uint32_t i = 0; i < r->cq_recs_size; i++)
        if (r->cq_recs[i].el)
            *cq_rec_slot(t, size, r->cq_recs[i].el) = r->cq_recs[i];
    au_free(r->cq_recs);
    r->cq_recs = t;
    r->cq_recs_size = size;
}

/* The content-box size of container a (-1 where it is not known), read
 * from the embedder and recorded. */
static void cq_size_of(AuCssResolver *r, const AuElement *a, float *w, float *h)
{
    *w = *h = -1;
    if (!r->cq_size)
        return;
    if (!r->cq_size(r->cq_size_ctx, a, w, h) || !(*w >= 0) || !(*h >= 0))
        *w = *h = -1; /* no box, or not a number */
    CqRec *rec = cq_rec_find(r, a);
    if (!rec) {
        if ((r->ncq_recs + 1) * 2 > r->cq_recs_size)
            cq_rec_rehash(r, r->cq_recs_size ? r->cq_recs_size * 2 : 16);
        rec = cq_rec_slot(r->cq_recs, r->cq_recs_size, a);
        rec->el = (AuElement *)a;
        au_node_ref(&rec->el->node);
        r->ncq_recs++;
    }
    rec->w = *w;
    rec->h = *h;
}

void au_css_resolver_forget_containers(AuCssResolver *r)
{
    for (uint32_t i = 0; i < r->cq_recs_size; i++)
        if (r->cq_recs[i].el)
            au_node_unref(&r->cq_recs[i].el->node);
    au_free(r->cq_recs);
    r->cq_recs = NULL;
    r->cq_recs_size = r->ncq_recs = 0;
}

static void mark_ancestors(AuElement *el);

static bool cq_differs(float a, float b) { return a > b + 0.01f || b > a + 0.01f; }

bool au_css_resolver_containers_changed(AuCssResolver *r, AuDocument *doc)
{
    bool changed = false, dropped = false;
    for (uint32_t i = 0; i < r->cq_recs_size; i++) {
        CqRec *rec = &r->cq_recs[i];
        AuElement *el = rec->el;
        if (!el)
            continue;
        const AuStyle *s = au_element_style(el);
        if (el->node.doc != doc || !au_node_is_connected(&el->node) || !s ||
            (s->rare->container_type == AU_CONTAINER_TYPE_NORMAL && !s->rare->container_name)) {
            /* gone, or no longer a container: what it styled is restyled
             * by whatever changed that */
            au_node_unref(&el->node);
            rec->el = NULL;
            r->ncq_recs--;
            dropped = true;
            continue;
        }
        float w = -1, h = -1;
        if (!r->cq_size || !r->cq_size(r->cq_size_ctx, el, &w, &h) || !(w >= 0) || !(h >= 0))
            w = h = -1;
        if (cq_differs(w, rec->w) || cq_differs(h, rec->h)) {
            rec->w = w;
            rec->h = h;
            el->state |= AU_STATE_STYLE_DIRTY;
            mark_ancestors(el);
            changed = true;
        }
    }
    if (dropped)
        cq_rec_rehash(r, r->cq_recs_size); /* open addressing: no holes in a probe run */
    return changed;
}

/* el's style, or the one being computed for it. */
static const AuStyle *cq_style(const AuCssResolver *r, const AuElement *el)
{
    return el == r->cq_pend_el ? r->cq_pend_style : au_element_style(el);
}

/* The axes a container of this type and writing mode can be queried in. */
static unsigned cq_axes_of(const AuStyle *s)
{
    int t = s->rare->container_type;
    if (t == AU_CONTAINER_TYPE_SIZE)
        return CQ_AXIS_WIDTH | CQ_AXIS_HEIGHT | CQ_AXIS_INLINE | CQ_AXIS_BLOCK;
    if (t == AU_CONTAINER_TYPE_INLINE_SIZE)
        return CQ_AXIS_INLINE | (s->inh->writing_mode == AU_WRITING_MODE_HORIZONTAL_TB ? CQ_AXIS_WIDTH : CQ_AXIS_HEIGHT);
    return 0;
}

static bool cq_has_name(const AuCssBlob *names, AuAtom name)
{
    if (!names)
        return false;
    uint32_t n;
    const AuCssValue *it = css_items(&names->value, &n);
    const char *want = au_atom_str(name);
    size_t wl = au_atom_len(name);
    for (uint32_t i = 0; i < n; i++) {
        const char *s = it[i].type == AU_CSSV_IDENT ? it[i].u.str
                        : it[i].type == AU_CSSV_KEYWORD ? au_css_keyword_name((AuCssKeyword)it[i].kw) : NULL;
        size_t sl = it[i].type == AU_CSSV_IDENT ? it[i].n : s ? strlen(s) : 0;
        if (s && sl == wl && memcmp(s, want, wl) == 0)
            return true;
    }
    return false;
}

/* Whether a is a container with the name (0: any) whose type allows the
 * axes in mask. A mask of 0 takes a name alone, of any type; CQ_ANY_TYPE
 * takes any size container. */
#define CQ_ANY_TYPE 0x10u
static bool cq_qualifies(const AuCssResolver *r, const AuElement *a, AuAtom name, unsigned mask)
{
    const AuStyle *s = cq_style(r, a);
    if (!s || (name && !cq_has_name(s->rare->container_name, name)))
        return false;
    if (!mask)
        return true;
    unsigned axes = cq_axes_of(s);
    return axes && !(mask & ~axes & ~CQ_ANY_TYPE);
}

static CqNear *cq_near_slot(AuCssResolver *r, const AuElement *el, AuAtom name, unsigned mask)
{
    uint32_t i = (cq_hash(el, CQ_NEAR_SIZE) ^ (name * 31u + mask * 7u)) & (CQ_NEAR_SIZE - 1);
    return &r->cq_near[i];
}

/* The nearest flat-tree ancestor-or-self of start that qualifies, or
 * NULL. Within a style pass ancestors are styled before descendants, so
 * an answer recorded for an ancestor stays good and ends the walk. */
static const AuElement *cq_find(AuCssResolver *r, const AuElement *start, AuAtom name, unsigned mask)
{
    bool memo = r->cur_el != NULL;
    if (memo && !r->cq_near)
        r->cq_near = au_calloc(CQ_NEAR_SIZE * sizeof *r->cq_near);
    if (!r->cq_near)
        memo = false;
    const AuElement *res = NULL;
    for (const AuElement *a = start; a; a = flat_parent_element(a)) {
        if (cq_qualifies(r, a, name, mask)) {
            res = a;
            break;
        }
        if (memo) {
            const CqNear *n = cq_near_slot(r, a, name, mask);
            if (n->el == a && n->gen == r->cq_gen && n->name == name && n->mask == mask) {
                res = n->res;
                break;
            }
        }
    }
    if (memo && start) {
        CqNear *n = cq_near_slot(r, start, name, mask);
        n->el = start;
        n->res = res;
        n->name = name;
        n->mask = mask;
        n->gen = r->cq_gen;
    }
    return res;
}

/* Evaluates one @container condition for the elements whose search for a
 * container starts at start (their parent; for a pseudo-element, its
 * element). */
static bool cq_eval_one(AuCssResolver *r, const CssContainerQuery *q, const AuElement *start)
{
    /* a name alone: the nearest container with it */
    const AuElement *a = cq_find(r, start, q->name, q->cond ? q->axes | CQ_ANY_TYPE : 0);
    if (!a)
        return false;
    if (!q->cond)
        return true;
    const AuStyle *s = cq_style(r, a);
    unsigned axes = cq_axes_of(s);
    CssCqEnv e;
    cq_size_of(r, a, &e.w, &e.h);
    e.horizontal = s->inh->writing_mode == AU_WRITING_MODE_HORIZONTAL_TB;
    if (!(axes & CQ_AXIS_BLOCK)) {
        /* an inline-size container: its block size is not queryable */
        if (e.horizontal)
            e.h = -1;
        else
            e.w = -1;
    }
    e.em = s->inh->font_size;
    e.rem = r->root_style ? r->root_style->inh->font_size : 16;
    e.dev = &r->dev;
    return css_container_eval(q, &e);
}

/* Whether the @container rules around an entry (innermost c) hold for
 * el, or for its pseudo-element with self. */
static bool cq_match(AuCssResolver *r, uint32_t c, const AuElement *el, bool self)
{
    const AuElement *start = self ? el : flat_parent_element(el);
    if (!start)
        return false;
    for (uint32_t guard = 0; c && c < r->ncq && guard < 64; guard++) {
        CqCond *cd = &r->cq_conds[c];
        bool res;
        if (r->cur_el && cd->memo_el == start && cd->memo_gen == r->cq_gen) {
            res = cd->memo;
        } else {
            res = cq_eval_one(r, cd->q, start);
            cd->memo_el = start;
            cd->memo_gen = r->cq_gen;
            cd->memo = res;
        }
        if (!res)
            return false;
        c = cd->parent;
    }
    return true;
}

/* A pseudo-element's container search starts at its element. */
static bool cq_self(uint8_t pseudo)
{
    return pseudo != AU_CSS_PSEUDO_NONE && pseudo != AU_CSS_PSEUDO_SLOTTED && pseudo != AU_CSS_PSEUDO_PART;
}

bool css_cq_axis_size(CssComputeCtx *c, int axis, float *px)
{
    AuCssResolver *r = c->cq;
    const AuElement *start = c->el ? (c->cq_self ? c->el : flat_parent_element(c->el)) : NULL;
    const AuElement *a = start ? cq_find(r, start, 0, (unsigned)axis) : NULL;
    if (a) {
        const AuStyle *s = cq_style(r, a);
        float w, h;
        cq_size_of(r, a, &w, &h);
        bool horiz = s->inh->writing_mode == AU_WRITING_MODE_HORIZONTAL_TB;
        bool wide = axis == CQ_AXIS_WIDTH || (axis == CQ_AXIS_INLINE && horiz) || (axis == CQ_AXIS_BLOCK && !horiz);
        float v = wide ? w : h;
        if (v < 0)
            return false; /* not laid out yet: the viewport, until the next round */
        *px = v;
        return true;
    }
    return false;
}

/* ---- Matching ---- */

#define KEY_LAYER_SHIFT 45 /* key >> KEY_LAYER_SHIFT: level, context, attached and layer */
#define KEY_ORDER_MAX 0x1FFFFFu

/* ctx: 0..7, larger wins; ctx_normal and ctx_important give it from the
 * context code of a matched entry. */
static uint64_t make_key(int level, int ctx, int attached, uint32_t layer, uint32_t spec, uint32_t order)
{
    if (layer > 4095)
        layer = 4095;
    if (order > KEY_ORDER_MAX)
        order = KEY_ORDER_MAX;
    return ((uint64_t)level << 61) | ((uint64_t)ctx << 58) | ((uint64_t)attached << 57) |
           ((uint64_t)layer << KEY_LAYER_SHIFT) | ((uint64_t)(spec & 0xFFFFFF) << 21) | order;
}

/* Context codes: CTX_OWN for the element's tree, below it the trees
 * outside (::part() rules), above it the trees inside (:host, then
 * ::slotted() along the chain of slots). Normal declarations of an outer
 * tree win, important ones of an inner tree. */
static int ctx_normal(int code) { return 7 - code; }
static int ctx_important(int code) { return code; }

static void push_matched(AuCssResolver *r, const AuCssDeclBlock *b, uint64_t kn, uint64_t ki)
{
    Matched *m = CSS_PUSH(r->matched, r->nmatched, r->matched_cap);
    m->decls = b;
    m->key_n = kn;
    m->key_i = ki;
}

/* The declarations of an entry's rule, parsed now if they are still
 * text. */
static const AuCssDeclBlock *entry_decls(AuCssResolver *r, RuleEntry *e)
{
    if (e->body & 1)
        e->body = (uintptr_t)css_rule_decls(r->walked[e->sheet], (const CssRule *)(e->body & ~(uintptr_t)1));
    return (const AuCssDeclBlock *)e->body;
}

/* Which entries a pass over the buckets takes: with r->scoping, those of
 * scope (and with any_too the user agent's and user's), each recorded
 * with context code ctx. */
typedef struct {
    uint16_t scope;
    bool any_too;
    uint32_t ctx; /* code << ME_CTX_SHIFT */
} Pass;

static void match_bucket(AuCssResolver *r, uint32_t start, uint32_t count, const AuElement *el, const CssMatchCtx *mc,
                         uint8_t want, uint16_t *pseudo_bits, const Pass *ps)
{
    const uint16_t *walked = r->scoping ? r->walked_scope : NULL; /* NULL: every entry counts */
    const uint16_t *ecq = r->entry_cq; /* NULL without @container rules */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ei = r->sorted[start + i];
        const RuleEntry *e = &r->entries[ei];
        if (walked) {
            uint16_t sc = walked[e->sheet];
            if (sc != ps->scope && !(sc == SCOPE_ANY && ps->any_too))
                continue;
        }
        if (e->pseudo != want) {
            if (want != AU_CSS_PSEUDO_NONE || !pseudo_bits || e->pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT)
                continue;
            /* The ::before, ::after, ::marker and ::placeholder styles hang off the
             * element's style, so the rules that give them are part of
             * what a shared style stands for: find them all. */
            bool attached = e->pseudo == AU_CSS_PSEUDO_BEFORE || e->pseudo == AU_CSS_PSEUDO_AFTER ||
                            e->pseudo == AU_CSS_PSEUDO_MARKER || e->pseudo == AU_CSS_PSEUDO_PLACEHOLDER;
            if (!attached && (*pseudo_bits & (1u << e->pseudo)))
                continue; /* already known to match */
            if (e->hash && mc->bloom && !css_bloom_may_contain(mc->bloom, e->hash))
                continue;
            /* An entry inside @container counts without its condition: el
             * may be the container, and its style is not known yet; the
             * pseudo-element's own cascade evaluates it. */
            if (css_selector_match(e->sel, el, mc)) {
                if (ecq && ecq[ei])
                    r->cq_pseudo = true; /* its result depends on el: el's style is not shared */
                *pseudo_bits |= (uint16_t)(1u << e->pseudo);
                if (attached)
                    *CSS_PUSH(r->pseudo_entries, r->npe, r->pe_cap) = ei | ps->ctx;
            }
            continue;
        }
        if (e->hash && mc->bloom && !css_bloom_may_contain(mc->bloom, e->hash))
            continue; /* an ancestor the selector names is not there */
        if (!css_selector_match(e->sel, el, mc))
            continue;
        if (ecq && ecq[ei] && !cq_match(r, ecq[ei], el, cq_self(e->pseudo)))
            continue;
        *CSS_PUSH(r->matched_entries, r->nme, r->me_cap) = ei | ps->ctx;
    }
}

/* The tree root of el: the style pass gives it for the element it
 * styles; anything else walks up. */
static const AuNode *root_of(const AuCssResolver *r, const AuElement *el)
{
    return r->cur_el == el ? r->cur_root : au_node_root(&el->node);
}

/* The names in a part attribute, or an exportparts mapping applied to
 * names (inner names to outer ones; "a" alone maps a to a). Up to max
 * names; returns the count. */
#define MAX_PART_NAMES 16
static uint32_t part_tokens(const char *v, AuAtom *out, uint32_t max)
{
    uint32_t n = 0;
    for (const char *p = v; *p && n < max;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\f' || *p == '\r')
            p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\f' && *p != '\r')
            p++;
        if (p > s)
            out[n++] = au_atom(s, (size_t)(p - s));
    }
    return n;
}

static uint32_t export_parts(const char *map, const AuAtom *in, uint32_t nin, AuAtom *out, uint32_t max)
{
    uint32_t n = 0;
    for (const char *p = map; *p && n < max;) {
        /* one "inner[:outer]" item up to a comma */
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *a = p, *colon = NULL;
        for (const char *q = p; q < e; q++)
            if (*q == ':' && !colon)
                colon = q;
        const char *ae = colon ? colon : e;
        const char *b = colon ? colon + 1 : a, *be = e;
        while (a < ae && au_is_space((unsigned char)*a))
            a++;
        while (ae > a && au_is_space((unsigned char)ae[-1]))
            ae--;
        while (b < be && au_is_space((unsigned char)*b))
            b++;
        while (be > b && au_is_space((unsigned char)be[-1]))
            be--;
        if (!colon) {
            b = a;
            be = ae;
        }
        if (ae > a && be > b) {
            AuAtom inner = au_atom(a, (size_t)(ae - a));
            for (uint32_t i = 0; i < nin; i++)
                if (in[i] == inner) {
                    out[n++] = au_atom(b, (size_t)(be - b));
                    break;
                }
        }
        p = *e ? e + 1 : e;
    }
    return n;
}

static void collect_buckets(AuCssResolver *r, const AuElement *el, const CssMatchCtx *mc, uint8_t want,
                            uint16_t *pseudo_bits, const Pass *ps)
{
    uint32_t s, n;
    bool q = r->quirks;
    if (el->id && map_get(&r->maps[0], q ? css_atom_lower(el->id) : el->id, &s, &n))
        match_bucket(r, s, n, el, mc, want, pseudo_bits, ps);
    for (uint32_t i = 0; i < el->nclasses; i++) {
        AuAtom c = q ? css_atom_lower(el->classes[i]) : el->classes[i];
        bool dup = false;
        if (q)
            for (uint32_t k = 0; k < i; k++)
                if (css_atom_lower(el->classes[k]) == c)
                    dup = true;
        if (!dup && map_get(&r->maps[1], c, &s, &n))
            match_bucket(r, s, n, el, mc, want, pseudo_bits, ps);
    }
    AuAtom tag = el->ns == AU_NS_HTML ? el->tag : css_atom_lower(el->tag);
    if (map_get(&r->maps[KEY_TAG], tag, &s, &n))
        match_bucket(r, s, n, el, mc, want, pseudo_bits, ps);
    if (r->maps[KEY_ATTR].size)
        for (uint32_t i = 0; i < el->nattrs; i++)
            if (map_get(&r->maps[KEY_ATTR], css_atom_lower(el->attrs[i].name), &s, &n))
                match_bucket(r, s, n, el, mc, want, pseudo_bits, ps);
    match_bucket(r, r->uni_start, r->uni_count, el, mc, want, pseudo_bits, ps);
}

/* The ::slotted() and ::part() entries of scope whose pseudo-element is
 * pe, matched against target (the slot, the host) for el. */
static void collect_shadow(AuCssResolver *r, const AuElement *target, const CssMatchCtx *mc, uint8_t pe, uint16_t scope,
                           int code)
{
    for (uint32_t i = 0; i < r->nshadow_entries; i++) {
        uint32_t ei = r->shadow_entries[i];
        const RuleEntry *e = &r->entries[ei];
        if (e->pseudo != pe || r->walked_scope[e->sheet] != scope)
            continue;
        if (e->hash && mc->bloom && !css_bloom_may_contain(mc->bloom, e->hash))
            continue;
        if (!css_selector_match(e->sel, target, mc))
            continue;
        /* the container is looked for above the slotted or part element */
        const AuElement *for_el = pe == AU_CSS_PSEUDO_SLOTTED ? mc->slotted : mc->part_el;
        if (r->entry_cq && r->entry_cq[ei] && (!for_el || !cq_match(r, r->entry_cq[ei], for_el, false)))
            continue;
        *CSS_PUSH(r->matched_entries, r->nme, r->me_cap) = ei | ((uint32_t)code << ME_CTX_SHIFT);
    }
}

/* The rules of the trees around el's own: its shadow tree's :host rules,
 * the ::slotted() rules of the slots it is assigned to, the ::part()
 * rules of the trees its parts reach. */
static void collect_scoped(AuCssResolver *r, const AuElement *el, const CssMatchCtx *mc, uint8_t want,
                           uint16_t *pseudo_bits, const AuNode *root)
{
    if (el->shadow_root) {
        uint16_t hs = scope_of_root(r, el->shadow_root);
        if (hs != SCOPE_NONE) {
            CssMatchCtx hm = *mc;
            hm.host = el;
            Pass ps = {hs, false, (uint32_t)(CTX_OWN + 1) << ME_CTX_SHIFT};
            /* :host rules are in the universal list (their compound names
             * no id, class or tag that the featureless host could match) */
            match_bucket(r, r->uni_start, r->uni_count, el, &hm, want, pseudo_bits, &ps);
        }
    }
    if (want != AU_CSS_PSEUDO_NONE)
        return;
    if (r->slotted_rules && el->node.parent && el->node.parent->type == AU_NODE_ELEMENT &&
        ((const AuElement *)el->node.parent)->shadow_root) {
        int code = CTX_OWN;
        for (AuElement *slot = au_node_assigned_slot(&el->node); slot && code < 7;
             slot = au_node_assigned_slot(&slot->node)) {
            code++;
            const AuNode *sroot = au_node_root(&slot->node);
            uint16_t sc = scope_of_root(r, sroot);
            if (sc == SCOPE_NONE || !au_is_shadow_root(sroot))
                continue;
            CssMatchCtx sm = *mc;
            sm.host = ((const AuShadowRoot *)sroot)->host;
            sm.slotted = el;
            collect_shadow(r, slot, &sm, AU_CSS_PSEUDO_SLOTTED, sc, code);
        }
    }
    const char *part;
    if (r->part_rules && au_is_shadow_root(root) && (part = au_element_get_attr(el, au_atom("part", 4))) != NULL) {
        AuAtom names[MAX_PART_NAMES], next[MAX_PART_NAMES];
        uint32_t nn = part_tokens(part, names, MAX_PART_NAMES);
        const AuElement *host = ((const AuShadowRoot *)root)->host;
        int code = CTX_OWN;
        while (host && nn && code > 0) {
            code--;
            const AuNode *hroot = au_node_root(&host->node);
            uint16_t sc = scope_of_root(r, hroot);
            if (sc != SCOPE_NONE) {
                CssMatchCtx pm = *mc;
                pm.host = au_is_shadow_root(hroot) ? ((const AuShadowRoot *)hroot)->host : NULL;
                pm.part_el = el;
                pm.part_names = names;
                pm.npart_names = nn;
                collect_shadow(r, host, &pm, AU_CSS_PSEUDO_PART, sc, code);
            }
            if (!au_is_shadow_root(hroot))
                break;
            const char *ex = au_element_get_attr(host, au_atom("exportparts", 11));
            nn = ex ? export_parts(ex, names, nn, next, MAX_PART_NAMES) : 0;
            memcpy(names, next, nn * sizeof *names);
            host = ((const AuShadowRoot *)hroot)->host;
        }
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
    r->cq_pseudo = false;
    Pass own = {0, true, (uint32_t)CTX_OWN << ME_CTX_SHIFT};
    if (r->scoping) {
        const AuNode *root = root_of(r, el);
        own.scope = scope_of_root(r, root);
        CssMatchCtx om = *mc;
        om.host = au_is_shadow_root(root) ? ((const AuShadowRoot *)root)->host : NULL;
        collect_buckets(r, el, &om, want, pseudo_bits, &own);
        collect_scoped(r, el, mc, want, pseudo_bits, root);
    } else {
        collect_buckets(r, el, mc, want, pseudo_bits, &own);
    }
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
        uint32_t v = r->matched_entries[i];
        RuleEntry *e = &r->entries[ME_INDEX(v)];
        int code = ME_CTX(v);
        int lvl_n = e->origin;
        int lvl_i = e->origin == AU_CSS_ORIGIN_AUTHOR ? 4 : e->origin == AU_CSS_ORIGIN_USER ? 5 : 6;
        uint32_t layer = (uint32_t)e->layer;
        push_matched(r, entry_decls(r, e), make_key(lvl_n, ctx_normal(code), 0, layer, e->spec, e->order),
                     make_key(lvl_i, ctx_important(code), 0, 4095 - layer, e->spec, e->order));
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
    /* A value that is one var() without a fallback is the value it
     * names: share that list instead of copying it (themes define
     * hundreds of such aliases). The depth is counted as substitute
     * counts it. */
    {
        const CssToken *t = in->tk.t;
        uint32_t s = 0, e = in->tk.n;
        while (s < e && t[s].type == TK_WS)
            s++;
        while (e > s && t[e - 1].type == TK_WS)
            e--;
        if (s < e && t[s].type == TK_FUNCTION && t[s].kw == AU_K_var && t[s].len == e &&
            t[e - 1].type == TK_RPAREN) {
            uint32_t k = s + 1, ie = e - 1;
            while (k < ie && t[k].type == TK_WS)
                k++;
            uint32_t q = k + 1;
            while (q < ie && t[q].type == TK_WS)
                q++;
            if (k < ie && q == ie && t[k].type == TK_IDENT &&
                css_ident_is_custom_prop(css_tok_str(&in->tk, &t[k]), t[k].len)) {
                if (++vc->depth > 64) {
                    vc->depth--;
                    return NULL;
                }
                CssTokList *val = resolve_custom(vc, au_atom(css_tok_str(&in->tk, &t[k]), t[k].len));
                vc->depth--;
                return val && !val->has_var ? css_toklist_retain(val) : NULL;
            }
        }
    }
    CssTokens out = {0};
    if (!substitute(vc, &in->tk, 0, in->tk.n, &out)) {
        css_tokens_free(&out);
        return NULL;
    }
    css_tokens_rematch(&out);
    /* trim whitespace */
    uint32_t s = 0, e = out.n;
    while (s < e && out.t[s].type == TK_WS)
        s++;
    while (e > s && out.t[e - 1].type == TK_WS)
        e--;
    CssTokList *l = css_toklist_new(&out, s, e);
    css_tokens_free(&out);
    /* css_toklist_new looks for var() and attr(); a substituted value has
     * no var() left to substitute */
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
 * origin lower than the origin of thr; kind 2: level, context and layer
 * lower (key >> KEY_LAYER_SHIFT). */
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
            bool below =
                kind == 1 ? level_origin(k) < level_origin(thr) : (k >> KEY_LAYER_SHIFT) < (thr >> KEY_LAYER_SHIFT);
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
        push_matched(r, hints, make_key(2, ctx_normal(CTX_OWN), 0, 0, 0, 0),
                     make_key(4, ctx_important(CTX_OWN), 0, 4095, 0, 0));
    if (inl && inl->n)
        push_matched(r, inl, make_key(2, ctx_normal(CTX_OWN), 1, 4095, 0, 0),
                     make_key(4, ctx_important(CTX_OWN), 1, 4095, 0, 0));

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
    cs.cc.cq = r;
    cs.cc.cq_self = cq_self(pseudo);
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
    if (cs.cc.flags & 8)
        s->flags |= AU_STYLE_F_CONTAINER_UNITS;
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
    /* An element in the top layer carries a ::backdrop style
     * (attach_pseudos) and a forced position (css_compute_finish) that
     * the share key does not cover. */
    return !root && !(inl && inl->n) && !(hints && hints->n) &&
           !(el->state & (AU_STATE_MODAL | AU_STATE_POPOVER_OPEN));
}

/* ---- Element styles ---- */

static void prepare(AuCssResolver *r, const AuElement *el)
{
    bool quirks = el->node.doc && el->node.doc->quirks == AU_QUIRKS_FULL;
    if (r->index_dirty || quirks != r->quirks)
        index_build(r, quirks);
    /* elements of shadow trees must not match the document's rules */
    r->scoping = r->any_scoped || (el->node.doc && el->node.doc->has_shadow);
}

/* Whether el's style may come from the share cache: not for a host, a
 * slotted element or a part, whose rules come from other trees than its
 * siblings' (the share key holds only the matched entries). */
static bool share_scope_ok(const AuCssResolver *r, const AuElement *el)
{
    if (!r->scoping)
        return true;
    if (el->shadow_root)
        return false;
    const AuNode *p = el->node.parent;
    if (p && p->type == AU_NODE_ELEMENT && ((const AuElement *)p)->shadow_root)
        return false;
    return !(r->part_rules && au_element_get_attr(el, au_atom("part", 4)));
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
    bool eligible = share_eligible(el, inl, &hints, root) && share_scope_ok(r, el) && !r->cq_pseudo;
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
    if (eligible && !(s->flags & (AU_STYLE_F_HAS_ATTR | AU_STYLE_F_CONTAINER_UNITS))) {
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
    static const uint8_t kinds[] = {AU_CSS_PSEUDO_BEFORE, AU_CSS_PSEUDO_AFTER, AU_CSS_PSEUDO_MARKER,
                                    AU_CSS_PSEUDO_BACKDROP, AU_CSS_PSEUDO_PLACEHOLDER};
    /* ::placeholder exists for text fields and textareas (by tag, so a
     * style shared between two of them carries it either way) */
    bool field = el->ns == AU_NS_HTML && (el->tag == HN(input) || el->tag == HN(textarea));
    AuStyle **tail = &s->pseudos;
    /* el is a candidate container of its pseudo-elements, with the style
     * computed now (stored after this) */
    r->cq_pend_el = el;
    r->cq_pend_style = s;
    for (size_t i = 0; i < CSS_ARRAY_LEN(kinds); i++) {
        uint8_t k = kinds[i];
        if (k == AU_CSS_PSEUDO_MARKER ? !au_display_list_item(s->box->display) : !(s->pseudo_bits & (1u << k)))
            continue;
        /* ::backdrop exists only for an element in the top layer */
        if (k == AU_CSS_PSEUDO_BACKDROP && !(el->state & (AU_STATE_MODAL | AU_STATE_POPOVER_OPEN)))
            continue;
        if (k == AU_CSS_PSEUDO_PLACEHOLDER && !field)
            continue;
        AuStyle *p = au_css_compute_pseudo(r, el, s, (AuCssPseudo)k);
        if (p) {
            *tail = p;
            tail = &p->next;
            uint8_t own = p->flags & (AU_STYLE_F_HAS_ATTR | AU_STYLE_F_CONTAINER_UNITS);
            if (own) {
                /* content: attr(x) or a container unit differs per
                 * element: the element's style, which carries it, must
                 * not be shared */
                s->flags |= own;
                for (int i = 0; i < SHARE_SLOTS; i++)
                    if (r->share[i].style == s) {
                        au_free(r->share[i].entries);
                        au_style_release(r->share[i].style);
                        memset(&r->share[i], 0, sizeof r->share[i]);
                    }
            }
        }
    }
    r->cq_pend_el = NULL;
    r->cq_pend_style = NULL;
}

static void store_style(AuElement *el, AuStyle *s)
{
    au_style_release((AuStyle *)el->style);
    el->style = s;
}

typedef struct {
    AuElement *el;
    const AuNode *root; /* the root of el's tree (a document or shadow root) */
    bool forced;
} PathEntry;

/* Releases the styles of root's subtree and of the shadow trees in it,
 * iteratively (shadow roots go on a work list). */
static void clear_styles(AuNode *root)
{
    AuNode **work = NULL;
    uint32_t nwork = 0, cap = 0;
    for (;;) {
        for (AuNode *n = root; n; n = au_node_next(n, root)) {
            if (n->type != AU_NODE_ELEMENT)
                continue;
            AuElement *el = (AuElement *)n;
            au_style_release((AuStyle *)el->style);
            el->style = NULL;
            if (el->shadow_root)
                *CSS_PUSH(work, nwork, cap) = el->shadow_root;
        }
        if (!nwork)
            break;
        root = work[--nwork];
    }
    au_free(work);
}

/* el is restyled and has flat children: what its flat tree leaves out (a
 * host's light children that no slot takes, a slot's fallback while it
 * has assigned nodes) loses its style, so that nothing takes it for
 * shown and it is computed again when a slot takes it. */
static void clear_unshown(AuElement *el)
{
    if (el->shadow_root) {
        for (AuNode *c = el->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_ELEMENT && ((AuElement *)c)->style && !au_node_assigned_slot(c))
                clear_styles(c);
    } else if (au_is_html_element(&el->node, AU_ATOM_slot) && el->node.first_child) {
        AuNode *first = au_flat_first_child(&el->node);
        if (first && first->parent != &el->node)
            for (AuNode *c = el->node.first_child; c; c = c->next)
                if (c->type == AU_NODE_ELEMENT && ((AuElement *)c)->style)
                    clear_styles(c);
    }
}

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
    r->cq_gen++; /* container query memos are good for one pass */
    css_bloom_clear(&r->bloom);
    css_has_index_free(&r->has_idx);
    r->has_root = r->has_has ? &doc->node : NULL;
    r->root_style = NULL;
    PathEntry *path = NULL;
    uint32_t np = 0, pcap = 0;
    const AuNode *root = &doc->node;
    /* With shadow trees the pass walks the flat tree: a host's flat
     * children are its shadow tree's, a slot's are its assigned nodes,
     * and inheritance follows (CSS Scoping 3.4). */
    bool flat = doc->has_shadow;
    for (AuNode *n = root->first_child; n;) {
        if (n->type != AU_NODE_ELEMENT) {
            n = flat ? au_flat_next(n, root) : au_node_next(n, root);
            continue;
        }
        AuElement *el = (AuElement *)n;
        /* pop to the parent */
        const AuNode *fp = flat ? au_flat_parent(n) : n->parent;
        while (np && &path[np - 1].el->node != fp) {
            css_bloom_pop(&r->bloom, path[np - 1].el, r->quirks);
            np--;
        }
        const AuElement *pe = np ? path[np - 1].el : NULL;
        /* the root of el's tree: its parent's, a shadow root's, or for a
         * slotted element its host's (on the path above the slot) */
        const AuNode *troot = root;
        if (flat) {
            if (pe && n->parent == &pe->node)
                troot = path[np - 1].root;
            else if (n->parent && au_is_shadow_root(n->parent))
                troot = n->parent;
            else if (n->parent != root) {
                troot = NULL;
                for (uint32_t k = np; k-- > 0 && !troot;)
                    if (&path[k].el->node == n->parent)
                        troot = path[k].root;
                if (!troot)
                    troot = au_node_root(n);
            }
        }
        r->cur_el = el;
        r->cur_root = troot;
        bool forced = !only_dirty || (np && path[np - 1].forced) || (el->state & AU_STATE_STYLE_DIRTY) || !el->style;
        if (forced || (el->state & AU_STATE_STYLE_SELF_DIRTY)) {
            const AuStyle *ps = pe ? au_element_style(pe) : NULL;
            AuStyle *s = compute_element(r, el, ps, &r->bloom);
            if (!pe)
                r->root_style = s; /* its pseudo-elements may use rem */
            attach_pseudos(r, el, s);
            if (flat)
                clear_unshown(el);
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
        AuNode *first = flat ? au_flat_first_child(n) : n->first_child;
        bool descend = first != NULL && (!only_dirty || forced || sub_dirty);
        if (descend) {
            PathEntry *pe2 = CSS_PUSH(path, np, pcap);
            pe2->el = el;
            pe2->root = troot;
            pe2->forced = forced;
            css_bloom_push(&r->bloom, el, r->quirks);
            n = first;
        } else {
            n = flat ? au_flat_next_skip(n, root) : au_node_next_skip(n, root);
        }
    }
    r->cur_el = NULL;
    r->cur_root = NULL;
    au_free(path);
    share_clear(r);
    css_has_index_free(&r->has_idx);
    r->has_root = NULL;
    doc->style_names_n = 0; /* the class and id changes are styled */
    r->styled_once = true;
    r->full_restyle = false;
    if (!only_dirty)
        r->sheets_full = false;
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
    bool flat = doc->has_shadow;
    for (AuNode *n = root->first_child; n; n = flat ? au_flat_next(n, root) : au_node_next(n, root)) {
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
    if (r->sheets_full)
        r->full_restyle = true;
    if (r->index_dirty && r->styled_once && !r->full_restyle) {
        /* only shadow trees' sheets changed (sheets_changed_in marked
         * their hosts): the new index serves the marked elements */
        AuElement *de = au_document_element(doc);
        if (de)
            prepare(r, de);
        else
            r->full_restyle = true;
    }
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
    clear_styles(&doc->node);
}

static AuElement *parent_element(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_ELEMENT ? (AuElement *)p : NULL;
}

/* The parent in the flat tree, which the style pass and :hover follow. */
static AuElement *flat_parent_element(const AuElement *el)
{
    if (el->node.doc && el->node.doc->has_shadow)
        return au_flat_parent_element(&el->node);
    return parent_element(el);
}

static void mark_ancestors(AuElement *el)
{
    for (AuElement *p = flat_parent_element(el); p; p = flat_parent_element(p))
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
    /* the nearest common ancestor-or-self keeps the bit; :hover and
     * :active follow the flat tree */
    AuElement *a = from, *b = to;
    uint32_t da = 0, db = 0;
    for (AuElement *p = a; p; p = flat_parent_element(p))
        da++;
    for (AuElement *p = b; p; p = flat_parent_element(p))
        db++;
    for (; da > db; da--)
        a = flat_parent_element(a);
    for (; db > da; db--)
        b = flat_parent_element(b);
    while (a != b) {
        a = flat_parent_element(a);
        b = flat_parent_element(b);
    }
    AuElement *common = a;
    for (AuElement *p = from; p && p != common; p = flat_parent_element(p))
        if (p->state & bit) {
            p->state &= ~bit;
            if (r)
                au_css_invalidate_state(r, p, bit);
        }
    for (AuElement *p = to; p && p != common; p = flat_parent_element(p))
        if (!(p->state & bit)) {
            p->state |= bit;
            if (r)
                au_css_invalidate_state(r, p, bit);
        }
    /* ancestors that lost the bit while detached get it back */
    for (AuElement *p = common; p && to; p = flat_parent_element(p))
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

/* ---- Child insertions and removals ----
 * As Blink scopes them (Element::ChildrenChanged and the
 * ChildrenAffectedBy flags), from what the rule index found when it was
 * built (deps_build): the inserted subtree, the parent for :empty, the
 * following or preceding siblings for positional pseudo-classes and
 * sibling combinators, and the :has() anchors among the ancestors and
 * their preceding siblings. */

static void dep_apply_mask(AuCssResolver *r, AuElement *el, uint8_t mask)
{
    for (uint8_t rel = REL_SELF; rel <= REL_ANY && !r->full_restyle; rel++)
        if (mask & (1u << rel))
            dep_apply(r, el, rel);
}

static AuElement *elem_at_or_after(AuNode *n)
{
    for (; n; n = n->next)
        if (n->type == AU_NODE_ELEMENT)
            return (AuElement *)n;
    return NULL;
}

static AuElement *elem_at_or_before(AuNode *n)
{
    for (; n; n = n->prev)
        if (n->type == AU_NODE_ELEMENT)
            return (AuElement *)n;
    return NULL;
}

/* The anchors of :has() a change below parent can reach: parent and its
 * ancestors, and for arguments that start with + or ~ the preceding
 * siblings of the change point (before is the node before it) and of
 * each of those. */
static void has_anchors_fire(AuCssResolver *r, AuElement *parent, AuNode *before)
{
    for (uint32_t i = 0; i < r->nhas_anchors && !r->full_restyle; i++) {
        const HasAnchor *h = &r->has_anchors[i];
        if (h->rel == REL_ANY) {
            r->full_restyle = true;
            return;
        }
        if (h->sib)
            for (AuNode *p = before; p; p = p->prev)
                if (p->type == AU_NODE_ELEMENT && dep_key_match(r, h->anchor_kind, h->anchor, (AuElement *)p))
                    dep_apply(r, (AuElement *)p, h->rel);
        for (AuElement *a = parent; a && !r->full_restyle; a = parent_element(a)) {
            if (dep_key_match(r, h->anchor_kind, h->anchor, a))
                dep_apply(r, a, h->rel);
            if (h->sib)
                for (AuNode *p = a->node.prev; p; p = p->prev)
                    if (p->type == AU_NODE_ELEMENT && dep_key_match(r, h->anchor_kind, h->anchor, (AuElement *)p))
                        dep_apply(r, (AuElement *)p, h->rel);
        }
    }
}

static bool is_form_owner(const AuElement *a)
{
    return a->ns == AU_NS_HTML && (a->tag == HN(form) || a->tag == HN(fieldset) || a->tag == HN(select) ||
                                   a->tag == HN(datalist) || a->tag == HN(optgroup) || a->tag == HN(textarea) ||
                                   a->tag == HN(option));
}

/* Pseudo-classes of form elements that follow their descendants: a
 * form's or fieldset's :valid and :invalid, a select's options' :checked
 * (the first option is selected when none says so), a select's own
 * validity, a textarea's :placeholder-shown (its value is its text). */
static void form_ancestors(AuCssResolver *r, AuElement *parent)
{
    const uint32_t bits = STATE_OTHER | AU_STATE_CHECKED | AU_STATE_CHECKED_SET | AU_STATE_INDETERMINATE;
    if (!(r->state_used & bits))
        return;
    for (AuElement *a = parent; a && !r->full_restyle; a = parent_element(a)) {
        if (!is_form_owner(a))
            continue;
        au_css_invalidate_state(r, a, bits);
        if (a->tag == HN(select) && (r->state_used & (AU_STATE_CHECKED | AU_STATE_CHECKED_SET)))
            for (AuNode *n = a->node.first_child; n; n = au_node_next(n, &a->node))
                if (n->type == AU_NODE_ELEMENT && ((AuElement *)n)->tag == HN(option))
                    au_css_invalidate_state(r, (AuElement *)n, bits);
    }
}

static bool holds_control(AuNode *n)
{
    for (AuNode *c = n; c; c = au_node_next(c, n))
        if (c->type == AU_NODE_ELEMENT && ((AuElement *)c)->ns == AU_NS_HTML &&
            (((AuElement *)c)->tag == HN(input) || ((AuElement *)c)->tag == HN(button)))
            return true;
    return false;
}

void au_css_invalidate_children(AuCssResolver *r, AuNode *parent, AuNode *child, AuNode *prev, AuNode *next,
                                bool inserted)
{
    if (!child || (child->type != AU_NODE_ELEMENT && child->type != AU_NODE_TEXT))
        return; /* comments and the like match nothing and count for nothing */
    if (!r->styled_once || r->index_dirty || r->full_restyle || r->deps_unknown || !parent ||
        (parent->type != AU_NODE_ELEMENT && !au_is_shadow_root(parent))) {
        r->full_restyle = true;
        return;
    }
    /* a child of a shadow root: its siblings are the shadow root's other
     * children; no element is its parent for :empty, :has() or forms */
    AuElement *pe = parent->type == AU_NODE_ELEMENT ? (AuElement *)parent : NULL;
    bool elem = child->type == AU_NODE_ELEMENT;
    if (inserted && elem) {
        /* The new subtree, whatever styles it had where it was before. */
        ((AuElement *)child)->state |= AU_STATE_STYLE_DIRTY;
        mark_ancestors((AuElement *)child);
    }
    AuNode *before = inserted ? child->prev : prev;
    AuNode *after = inserted ? child->next : next;
    if (r->st_empty && pe)
        dep_apply_mask(r, pe, r->st_empty);
    if (elem) {
        /* What a sibling rule marks from one element covers the same
         * marks from every element after it: those go on the first only,
         * so a long list costs its length, not its square. */
        const uint8_t own = (1u << REL_SELF) | (1u << REL_DESC) | (1u << REL_ANY);
        uint8_t fwd = r->st_forward | r->st_ind;
        bool first = true;
        if (fwd)
            for (AuElement *e = elem_at_or_after(after); e && !r->full_restyle; e = elem_at_or_after(e->node.next)) {
                dep_apply_mask(r, e, first ? fwd : (uint8_t)(fwd & own));
                first = false;
            }
        if (r->st_back) {
            AuElement *earliest = NULL;
            for (AuElement *e = elem_at_or_before(before); e && !r->full_restyle; e = elem_at_or_before(e->node.prev)) {
                dep_apply_mask(r, e, (uint8_t)(r->st_back & own));
                earliest = e;
            }
            if (earliest && !r->full_restyle)
                dep_apply_mask(r, earliest, (uint8_t)(r->st_back & ~own));
        }
        AuElement *adj = r->st_adj ? elem_at_or_after(after) : NULL;
        if (adj)
            dep_apply_mask(r, adj, r->st_adj);
        if (r->st_form && holds_control(child))
            r->full_restyle = true; /* the form's default button, a radio group */
    }
    if (r->nhas_anchors && !r->full_restyle && pe)
        has_anchors_fire(r, pe, before);
    if (!r->full_restyle && pe)
        form_ancestors(r, pe);
}

void au_css_invalidate_subtree(AuCssResolver *r, AuElement *el)
{
    if (!r->styled_once || r->index_dirty || r->full_restyle) {
        r->full_restyle = true;
        return;
    }
    el->state |= AU_STATE_STYLE_DIRTY;
    mark_ancestors(el);
}

/* The next element the style pass styles after c, in preorder of the
 * flat tree (or of the node tree in a document without shadow roots). */
static AuNode *next_styled(const AuNode *c, const AuNode *top, bool flat)
{
    return flat ? au_flat_next(c, top) : au_node_next(c, top);
}

/* Every computed longhand of s and of its pseudo-element styles. */
static void serialize_all(const AuStyle *s, AuBuf *out)
{
    for (const AuStyle *p = s; p; p = p == s ? s->pseudos : p->next) {
        au_buf_printf(out, "[%u]", (unsigned)p->pseudo);
        for (int k = 1; k < AU_PROP_LONGHAND_END; k++) {
            au_style_serialize_property(p, au_atom_cstr(au_css_property_name((AuCssProp)k)), out);
            au_buf_putc(out, ';');
        }
        au_buf_printf(out, "f%u b%u ", (unsigned)p->flags, (unsigned)p->pseudo_bits);
    }
}

bool au_css_check_restyle(AuCssResolver *r, AuDocument *doc, AuBuf *msg)
{
    const AuNode *top = &doc->node;
    bool flat = doc->has_shadow;
    uint32_t n = 0;
    for (AuNode *c = top->first_child; c; c = next_styled(c, top, flat))
        n += c->type == AU_NODE_ELEMENT;
    AuStyle **old = au_alloc((n ? n : 1) * sizeof *old);
    uint32_t i = 0;
    for (AuNode *c = top->first_child; c; c = next_styled(c, top, flat))
        if (c->type == AU_NODE_ELEMENT)
            old[i++] = au_style_retain((AuStyle *)((AuElement *)c)->style);
    /* A whole pass keeps an element's style object when its values are
     * equal: an object that changed may hold a difference. */
    style_pass(r, doc, false);
    bool ok = true;
    i = 0;
    AuBuf a, b;
    au_buf_init(&a);
    au_buf_init(&b);
    for (AuNode *c = top->first_child; c && i < n; c = next_styled(c, top, flat)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *el = (AuElement *)c;
        AuStyle *was = old[i++];
        if (ok && was != el->style) {
            au_buf_clear(&a);
            au_buf_clear(&b);
            if (was)
                serialize_all(was, &a);
            if (el->style)
                serialize_all(el->style, &b);
            if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0) {
                size_t k = 0;
                while (k < a.len && k < b.len && a.data[k] == b.data[k])
                    k++;
                size_t s = k > 40 ? k - 40 : 0;
                au_buf_printf(msg, "the style of <%s> differs from a whole restyle: ...%.*s | whole: ...%.*s\n",
                              au_atom_str(el->tag), (int)(a.len - s < 120 ? a.len - s : 120), a.data ? a.data + s : "",
                              (int)(b.len - s < 120 ? b.len - s : 120), b.data ? b.data + s : "");
                ok = false;
            }
        }
        au_style_release(was);
    }
    au_buf_free(&a);
    au_buf_free(&b);
    au_free(old);
    return ok;
}

void au_css_invalidate_text(AuCssResolver *r, AuNode *text)
{
    AuNode *parent = text ? text->parent : NULL;
    if (!parent || parent->type != AU_NODE_ELEMENT)
        return; /* text outside an element is not styled */
    if (!r->styled_once || r->index_dirty || r->full_restyle || r->deps_unknown) {
        r->full_restyle = true;
        return;
    }
    AuElement *pe = (AuElement *)parent;
    if (r->st_empty)
        dep_apply_mask(r, pe, r->st_empty);
    if (r->st_has_empty && !r->full_restyle)
        has_anchors_fire(r, pe, text->prev);
    if (!r->full_restyle)
        form_ancestors(r, pe);
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
