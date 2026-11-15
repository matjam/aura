/* Font sets: the faces the platform has, CSS font matching with the alias
 * table, and the per-character fallback chain of a font list. */
#include "font.h"

#include <string.h>

typedef struct Entry {
    char family[64];
    int weight;
    bool italic;
    unsigned flags; /* AU_FONT_* */
    int index; /* face index in its file */
    AuFontFace *face;
    AuFontFace *synth[3]; /* variants by AU_FONT_SYNTH_* - 1, made on first match */
    const void *data; /* when registered with data */
    size_t len;
    AuFontLoader load;
    void *ctx;
    bool failed;
} Entry;

/* The fallback cache: the face the global stage (fallback lists, then
 * loaded faces) found for a code point and style. */
#define FB_SLOTS 512
typedef struct FbSlot {
    uint32_t cp1;  /* code point + 1; 0 for an empty slot */
    uint16_t key;  /* weight / 100, italic, emoji */
    AuFontFace *face;
} FbSlot;

struct AuFontSet {
    Entry *e;
    size_t n, cap;
    char generic[AU_GENERIC_COUNT][64];
    FbSlot fb[FB_SLOTS];
    AuFontSetStats stats;
};

AuFontSet *au_fontset_new(void)
{
    return au_calloc(sizeof(AuFontSet));
}

void au_fontset_free(AuFontSet *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++) {
        for (int k = 0; k < 3; k++)
            au_font_face_free(s->e[i].synth[k]);
        au_font_face_free(s->e[i].face);
    }
    au_free(s->e);
    au_free(s);
}

static void copy_name(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static Entry *new_entry(AuFontSet *s)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->e = au_realloc(s->e, s->cap * sizeof(Entry));
    }
    /* A new face can change what the fallback stage finds. */
    memset(s->fb, 0, sizeof s->fb);
    Entry *e = &s->e[s->n++];
    memset(e, 0, sizeof *e);
    return e;
}

int au_fontset_add_data(AuFontSet *s, const void *data, size_t len)
{
    int n = au_font_face_count(data, len), added = 0;
    for (int i = 0; i < n; i++) {
        AuFontFace *f = au_font_face_new(data, len, i);
        if (!f)
            continue;
        Entry *e = new_entry(s);
        copy_name(e->family, sizeof e->family, f->family, strlen(f->family));
        e->weight = f->weight;
        e->italic = f->italic;
        e->index = i;
        e->face = f;
        e->data = data;
        e->len = len;
        added++;
    }
    return added;
}

void au_fontset_add_lazy(AuFontSet *s, const char *family, int weight, bool italic, int face_index, unsigned flags,
                         AuFontLoader load, void *ctx)
{
    Entry *e = new_entry(s);
    copy_name(e->family, sizeof e->family, family, strlen(family));
    e->weight = weight;
    e->italic = italic;
    e->index = face_index;
    e->flags = flags;
    e->load = load;
    e->ctx = ctx;
}

void au_fontset_set_generic(AuFontSet *s, AuGenericFamily g, const char *family)
{
    if ((unsigned)g < AU_GENERIC_COUNT)
        copy_name(s->generic[g], sizeof s->generic[g], family, strlen(family));
}

size_t au_fontset_count(const AuFontSet *s)
{
    return s->n;
}

const char *au_fontset_entry(const AuFontSet *s, size_t i, int *weight, bool *italic)
{
    if (i >= s->n)
        return NULL;
    if (weight)
        *weight = s->e[i].weight;
    if (italic)
        *italic = s->e[i].italic;
    return s->e[i].family;
}

unsigned au_fontset_entry_flags(const AuFontSet *s, size_t i)
{
    return i < s->n ? s->e[i].flags : 0;
}

bool au_fontset_entry_loaded(const AuFontSet *s, size_t i)
{
    return i < s->n && s->e[i].face != NULL;
}

bool au_fontset_has_family(const AuFontSet *s, const char *family, size_t len)
{
    for (size_t i = 0; i < s->n; i++)
        if (!s->e[i].failed && au_ieq(s->e[i].family, strlen(s->e[i].family), family, len))
            return true;
    return false;
}

void au_fontset_stats(const AuFontSet *s, AuFontSetStats *st)
{
    *st = s->stats;
    st->loaded = 0;
    st->loaded_bytes = 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->e[i].face) {
            st->loaded++;
            st->loaded_bytes += s->e[i].face->len;
        }
}

static bool load_entry(AuFontSet *s, Entry *e)
{
    if (e->face)
        return true;
    if (e->failed)
        return false;
    const void *data = NULL;
    size_t len = 0;
    if (e->load && e->load(e->ctx, &data, &len))
        e->face = au_font_face_new(data, len, e->index);
    if (!e->face)
        e->failed = true;
    else
        s->stats.loads++;
    return e->face != NULL;
}

/* CSS Fonts 4, font-weight matching: a smaller key is a better match. */
static int weight_key(int want, int have)
{
    if (have == want)
        return 0;
    if (want >= 400 && want <= 500) {
        if (have > want && have <= 500)
            return have - want;
        if (have < want)
            return 1000 + (want - have);
        return 2000 + (have - want);
    }
    if (want < 400) {
        if (have < want)
            return want - have;
        return 1000 + (have - want);
    }
    if (have > want)
        return have - want;
    return 1000 + (want - have);
}

/* The matched face f, or a variant of it that synthesizes the italic or
 * bold it lacks: italic when an italic or oblique style was asked for and
 * f is upright; bold when 600 or more was asked for and f is 500 or
 * lighter (Blink's rule). */
static AuFontFace *synthesize(Entry *e, int weight, bool italic)
{
    /* The entry's style, as registered, is what the face provides. */
    unsigned want = 0;
    if (italic && !e->italic)
        want |= AU_FONT_SYNTH_ITALIC;
    if (weight >= 600 && e->weight <= 500)
        want |= AU_FONT_SYNTH_BOLD;
    if (!want)
        return e->face;
    if (!e->synth[want - 1]) {
        e->synth[want - 1] = au_font_face_synthesize(e->face, want);
        if (e->italic)
            e->synth[want - 1]->italic = true;
    }
    return e->synth[want - 1];
}

/* The best face of family, loaded and with synthesized styles, or NULL. */
static AuFontFace *match_family(AuFontSet *s, const char *family, size_t flen, int weight, bool italic)
{
    for (;;) {
        Entry *best = NULL;
        int best_key = 0;
        for (size_t i = 0; i < s->n; i++) {
            Entry *e = &s->e[i];
            if (e->failed || !au_ieq(e->family, strlen(e->family), family, flen))
                continue;
            int key = weight_key(weight, e->weight) + (e->italic != italic ? 100000 : 0);
            if (!best || key < best_key) {
                best = e;
                best_key = key;
            }
        }
        if (!best)
            return NULL;
        if (load_entry(s, best))
            return synthesize(best, weight, italic);
        /* It failed to load: it is marked, so the next pass skips it. */
    }
}

/* ---- Family lists ---- */

typedef struct Name {
    const char *s;
    size_t n;
    bool quoted;
} Name;

/* Reads the next name of a comma-separated family list at *p. Unquoted
 * names are trimmed; quoted names are taken as written. */
static bool next_name(const char **p, Name *out)
{
    const char *q = *p;
    while (*q == ',' || au_is_space((unsigned char)*q))
        q++;
    if (!*q) {
        *p = q;
        return false;
    }
    if (*q == '"' || *q == '\'') {
        char quote = *q++;
        out->s = q;
        while (*q && *q != quote)
            q++;
        out->n = (size_t)(q - out->s);
        out->quoted = true;
        if (*q)
            q++;
        while (*q && *q != ',')
            q++;
    } else {
        out->s = q;
        while (*q && *q != ',')
            q++;
        const char *end = q;
        while (end > out->s && au_is_space((unsigned char)end[-1]))
            end--;
        out->n = (size_t)(end - out->s);
        out->quoted = false;
    }
    *p = q;
    return true;
}

/* The candidate families of a font-family list, in the order they are
 * tried: the named families as written; then the alias targets of each
 * named family; then each generic family (the user's preference, then its
 * alias row); then sans-serif as the default. Duplicates are dropped. */
#define MAX_CANDS 48
typedef struct Cands {
    Name v[MAX_CANDS];
    size_t n;
} Cands;

static void cand_add(Cands *c, const char *s, size_t n)
{
    while (n && au_is_space((unsigned char)*s))
        s++, n--;
    while (n && au_is_space((unsigned char)s[n - 1]))
        n--;
    if (!n || n >= 64 || c->n == MAX_CANDS)
        return;
    for (size_t i = 0; i < c->n; i++)
        if (au_ieq(c->v[i].s, c->v[i].n, s, n))
            return;
    c->v[c->n].s = s;
    c->v[c->n].n = n;
    c->v[c->n].quoted = false;
    c->n++;
}

static void add_generic(AuFontSet *s, Cands *c, const AuFontAlias *g, int depth);

/* Adds the targets of a comma-separated list; a target naming a generic
 * family expands to it (one level deep). */
static void add_targets(AuFontSet *s, Cands *c, const char *targets, int depth)
{
    const char *p = targets;
    Name nm;
    while (next_name(&p, &nm)) {
        const AuFontAlias *g = depth == 0 ? au_font_generic(nm.s, nm.n) : NULL;
        if (g)
            add_generic(s, c, g, depth + 1);
        else
            cand_add(c, nm.s, nm.n);
    }
}

static void add_generic(AuFontSet *s, Cands *c, const AuFontAlias *g, int depth)
{
    if (g->generic >= 0) {
        const char *pref = s->generic[g->generic];
        if (!pref[0] && g->generic == AU_GENERIC_SYSTEM_UI)
            pref = s->generic[AU_GENERIC_SANS_SERIF];
        if (!pref[0] && (g->generic == AU_GENERIC_CURSIVE || g->generic == AU_GENERIC_FANTASY))
            pref = s->generic[AU_GENERIC_SERIF];
        if (pref[0])
            cand_add(c, pref, strlen(pref));
    }
    add_targets(s, c, g->targets, depth);
}

static void build_cands(AuFontSet *s, const Name *names, size_t n, Cands *c)
{
    c->n = 0;
    /* A generic family ends a segment of named families: "Roboto, Arial,
     * sans-serif" tries Roboto, Arial, Roboto's aliases, then sans-serif,
     * so an installed family the page names wins over an alias of an
     * earlier one, and a generic still wins over what follows it. */
    size_t seg = 0;
    for (size_t i = 0; i <= n; i++) {
        const AuFontAlias *g = i < n && !names[i].quoted ? au_font_generic(names[i].s, names[i].n) : NULL;
        if (i < n && !g)
            continue;
        for (size_t k = seg; k < i; k++)
            cand_add(c, names[k].s, names[k].n);
        for (size_t k = seg; k < i; k++) {
            const AuFontAlias *a = au_font_alias(names[k].s, names[k].n);
            if (a && !au_font_generic(names[k].s, names[k].n))
                add_targets(s, c, a->targets, 0);
        }
        if (g)
            add_generic(s, c, g, 0);
        seg = i + 1;
    }
    add_generic(s, c, au_font_generic("sans-serif", 10), 0);
}

static AuFontFace *any_face(AuFontSet *s, int weight, bool italic)
{
    for (size_t i = 0; i < s->n; i++)
        if (load_entry(s, &s->e[i]))
            return synthesize(&s->e[i], weight, italic);
    return NULL;
}

static AuFontFace *match_names(AuFontSet *s, const Name *names, size_t n, int weight, bool italic)
{
    Cands c;
    build_cands(s, names, n, &c);
    for (size_t i = 0; i < c.n; i++) {
        AuFontFace *f = match_family(s, c.v[i].s, c.v[i].n, weight, italic);
        if (f)
            return f;
    }
    return any_face(s, weight, italic);
}

#define MAX_NAMES 16

AuFontFace *au_fontset_match_list(AuFontSet *s, const char *const *families, size_t n, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    for (size_t i = 0; i < n && k < MAX_NAMES; i++) {
        names[k].s = families[i];
        names[k].n = strlen(families[i]);
        names[k].quoted = false;
        k++;
    }
    return match_names(s, names, k, weight, italic);
}

AuFontFace *au_fontset_match(AuFontSet *s, const char *families, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    const char *p = families ? families : "";
    while (k < MAX_NAMES && next_name(&p, &names[k]))
        k++;
    return match_names(s, names, k, weight, italic);
}

/* ---- Font lists and per-character fallback ---- */

#define LIST_SLOTS 64
typedef struct ListCand {
    char name[64];
    int state; /* 0 not tried, 1 matched, -1 no face */
    AuFontFace *face;
} ListCand;

struct AuFontList {
    AuFontSet *set;
    int weight;
    bool italic;
    AuFontFace *primary;
    size_t ncand;
    ListCand *cand;
    struct {
        uint32_t cp1; /* code point + 1, times 2 plus emoji */
        AuFontFace *face;
    } cache[LIST_SLOTS];
};

static AuFontList *list_new(AuFontSet *s, const Name *names, size_t n, int weight, bool italic)
{
    AuFontList *l = au_calloc(sizeof *l);
    l->set = s;
    l->weight = weight;
    l->italic = italic;
    Cands c;
    build_cands(s, names, n, &c);
    l->cand = au_calloc((c.n ? c.n : 1) * sizeof *l->cand);
    l->ncand = c.n;
    for (size_t i = 0; i < c.n; i++)
        copy_name(l->cand[i].name, sizeof l->cand[i].name, c.v[i].s, c.v[i].n);
    for (size_t i = 0; i < l->ncand && !l->primary; i++) {
        ListCand *lc = &l->cand[i];
        lc->face = match_family(s, lc->name, strlen(lc->name), weight, italic);
        lc->state = lc->face ? 1 : -1;
        l->primary = lc->face;
    }
    if (!l->primary)
        l->primary = any_face(s, weight, italic);
    return l;
}

AuFontList *au_fontlist_new(AuFontSet *s, const char *const *families, size_t n, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    for (size_t i = 0; i < n && k < MAX_NAMES; i++) {
        names[k].s = families[i];
        names[k].n = strlen(families[i]);
        names[k].quoted = false;
        k++;
    }
    return list_new(s, names, k, weight, italic);
}

AuFontList *au_fontlist_parse(AuFontSet *s, const char *families, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    const char *p = families ? families : "";
    while (k < MAX_NAMES && next_name(&p, &names[k]))
        k++;
    return list_new(s, names, k, weight, italic);
}

void au_fontlist_free(AuFontList *l)
{
    if (!l)
        return;
    au_free(l->cand);
    au_free(l);
}

AuFontFace *au_fontlist_primary(const AuFontList *l)
{
    return l ? l->primary : NULL;
}

/* True when every code point of cps is in f. */
static bool has_all(const AuFontFace *f, const uint32_t *cps, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!au_font_glyph(f, cps[i]))
            return false;
    return true;
}

/* The first face of a comma-separated fallback list that has cps. */
static AuFontFace *from_names(AuFontSet *s, const char *names, const uint32_t *cps, size_t n, int weight,
                              bool italic)
{
    const char *p = names;
    Name nm;
    while (next_name(&p, &nm)) {
        AuFontFace *f = match_family(s, nm.s, nm.n, weight, italic);
        if (f && has_all(f, cps, n))
            return f;
    }
    return NULL;
}

/* The global stage. Emoji: the emoji fallback list only. Text: the text
 * fallback list, the emoji list, then any face already loaded (none is
 * loaded only to look). */
static AuFontFace *global_face(AuFontSet *s, const uint32_t *cps, size_t n, int weight, bool italic, bool emoji)
{
    if (emoji)
        return from_names(s, au_font_emoji_fallbacks(), cps, n, weight, italic);
    AuFontFace *f = from_names(s, au_font_text_fallbacks(), cps, n, weight, italic);
    if (!f)
        f = from_names(s, au_font_emoji_fallbacks(), cps, n, weight, italic);
    for (size_t i = 0; !f && i < s->n; i++) {
        Entry *e = &s->e[i];
        if (e->face && has_all(e->face, cps, n))
            f = synthesize(e, weight, italic);
    }
    return f;
}

static AuFontFace *global_cached(AuFontSet *s, uint32_t cp, int weight, bool italic, bool emoji)
{
    uint16_t key = (uint16_t)(((weight / 100) << 2) | (italic ? 2 : 0) | (emoji ? 1 : 0));
    uint32_t h = (cp * 2654435761u) ^ (key * 40503u);
    FbSlot *slot = &s->fb[(h >> 7) & (FB_SLOTS - 1)];
    if (slot->cp1 == cp + 1 && slot->key == key) {
        s->stats.fallback_hits++;
        return slot->face;
    }
    s->stats.fallback_misses++;
    AuFontFace *f = global_face(s, &cp, 1, weight, italic, emoji);
    slot->cp1 = cp + 1;
    slot->key = key;
    slot->face = f;
    return f;
}

/* The faces of the list itself, in order, that have cps. */
static AuFontFace *from_list(AuFontList *l, const uint32_t *cps, size_t n)
{
    for (size_t i = 0; i < l->ncand; i++) {
        ListCand *lc = &l->cand[i];
        if (lc->state == 0) {
            lc->face = match_family(l->set, lc->name, strlen(lc->name), l->weight, l->italic);
            lc->state = lc->face ? 1 : -1;
        }
        if (lc->state > 0 && has_all(lc->face, cps, n))
            return lc->face;
    }
    return NULL;
}

AuFontFace *au_fontlist_face_for_cluster(AuFontList *l, const uint32_t *cps, size_t n, bool emoji)
{
    if (!l || !l->primary || !n)
        return NULL;
    if (!emoji && has_all(l->primary, cps, n))
        return l->primary;
    uint32_t key = 0;
    unsigned slot = 0;
    if (n == 1) {
        key = (cps[0] + 1) * 2 + (emoji ? 1 : 0);
        slot = (key * 2654435761u) >> 26;
        if (l->cache[slot].cp1 == key)
            return l->cache[slot].face;
    }
    AuFontFace *f = NULL;
    if (emoji)
        f = n == 1 ? global_cached(l->set, cps[0], l->weight, l->italic, true)
                   : global_face(l->set, cps, n, l->weight, l->italic, true);
    if (!f)
        f = from_list(l, cps, n);
    if (!f)
        f = n == 1 ? global_cached(l->set, cps[0], l->weight, l->italic, false)
                   : global_face(l->set, cps, n, l->weight, l->italic, false);
    if (n == 1) {
        l->cache[slot].cp1 = key;
        l->cache[slot].face = f;
    }
    return f;
}

AuFontFace *au_fontlist_face_for(AuFontList *l, uint32_t cp, bool emoji)
{
    return au_fontlist_face_for_cluster(l, &cp, 1, emoji);
}
