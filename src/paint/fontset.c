/* Font sets: the faces the platform has, CSS font matching with the alias
 * table, variable-font instances, web fonts layered over the platform's
 * set, and the per-character fallback chain of a font list. */
#include "font_internal.h"

#include <math.h>
#include <string.h>

/* An instance of a variable entry: the axis positions it was made for and
 * the synthesized styles on top. Instances live as long as the set (font
 * lists hold them), so they are never evicted. */
#define INST_VARS 12
#define MAX_INST 64
typedef struct Inst {
    AuFontVariation v[INST_VARS];
    uint8_t nv, synth;
    AuFontFace *face;
} Inst;

typedef struct Entry {
    char family[64];
    int weight;     /* as registered (a variable face: its default) */
    int wmin, wmax; /* the weights it serves: a variable face's wght range */
    bool italic;
    bool var;       /* the loaded face has variation axes */
    unsigned flags; /* AU_FONT_* */
    int index; /* face index in its file */
    AuFontFace *face;
    AuFontFace *synth[3]; /* variants by AU_FONT_SYNTH_* - 1, made on first match */
    Inst *inst;
    size_t ninst;
    const void *data; /* when registered with data */
    size_t len;
    void *owned;      /* web font data the set frees */
    uint32_t *ranges; /* unicode-range pairs, NULL for all */
    size_t nranges;
    AuFontLoader load;
    const AuFontFileOps *ops; /* a file read whole or paged (au_fontset_add_file) */
    void *ctx;
    AuFontPager *pager; /* the loaded face is paged */
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
    AuFontSet *parent;
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

AuFontSet *au_fontset_new_layered(AuFontSet *parent)
{
    AuFontSet *s = au_calloc(sizeof(AuFontSet));
    s->parent = parent;
    return s;
}

void au_fontset_free(AuFontSet *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++) {
        Entry *e = &s->e[i];
        for (size_t k = 0; k < e->ninst; k++)
            if (e->inst[k].face != e->face)
                au_font_face_free(e->inst[k].face);
        au_free(e->inst);
        for (int k = 0; k < 3; k++)
            au_font_face_free(e->synth[k]);
        au_font_face_free(e->face);
        au_font_pager_free(e->pager);
        au_free(e->ranges);
        au_free(e->owned);
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

/* A loaded face's variation axes widen the weights the entry serves. */
static void note_axes(Entry *e)
{
    AuFontFace *f = e->face;
    e->var = au_font_axis_count(f) > 0;
    int w = au_font_axis_find(f, AU_FONT_TAG('w', 'g', 'h', 't'));
    if (w >= 0 && !(e->flags & AU_FONT_WEB)) {
        const AuFontAxis *a = au_font_axis(f, w);
        e->wmin = (int)ceilf(a->min);
        e->wmax = (int)floorf(a->max);
        if (e->wmin < 1)
            e->wmin = 1;
        if (e->wmax > 1000)
            e->wmax = 1000;
        if (e->wmin > e->wmax)
            e->wmin = e->wmax = e->weight;
    }
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
        e->weight = e->wmin = e->wmax = f->weight;
        e->italic = f->italic;
        e->index = i;
        e->face = f;
        e->data = data;
        e->len = len;
        note_axes(e);
        added++;
    }
    return added;
}

bool au_fontset_add_web(AuFontSet *s, void *data, size_t len, const char *family, int wmin, int wmax, bool italic,
                        const uint32_t *ranges, size_t nranges)
{
    AuFontFace *f = data ? au_font_face_new(data, len, 0) : NULL;
    if (!f) {
        au_free(data);
        return false;
    }
    Entry *e = new_entry(s);
    copy_name(e->family, sizeof e->family, family, strlen(family));
    if (wmin < 1)
        wmin = 1;
    if (wmax > 1000)
        wmax = 1000;
    if (wmin > wmax)
        wmin = wmax = 400;
    e->wmin = wmin;
    e->wmax = wmax;
    e->weight = f->weight >= wmin && f->weight <= wmax ? f->weight : wmin;
    e->italic = italic;
    e->flags = AU_FONT_WEB;
    e->face = f;
    e->data = e->owned = data;
    e->len = len;
    if (ranges && nranges) {
        if (nranges > 256)
            nranges = 256;
        e->ranges = au_alloc(nranges * 2 * sizeof(uint32_t));
        memcpy(e->ranges, ranges, nranges * 2 * sizeof(uint32_t));
        e->nranges = nranges;
    }
    note_axes(e);
    memset(s->fb, 0, sizeof s->fb); /* the new face may cover cached code points */
    return true;
}

void au_fontset_add_lazy(AuFontSet *s, const char *family, int weight, bool italic, int face_index, unsigned flags,
                         AuFontLoader load, void *ctx)
{
    Entry *e = new_entry(s);
    copy_name(e->family, sizeof e->family, family, strlen(family));
    e->weight = e->wmin = e->wmax = weight;
    e->italic = italic;
    e->index = face_index;
    e->flags = flags;
    e->load = load;
    e->ctx = ctx;
}

void au_fontset_add_file(AuFontSet *s, const char *family, int weight, bool italic, int face_index, unsigned flags,
                         const AuFontFileOps *ops, void *ctx)
{
    au_fontset_add_lazy(s, family, weight, italic, face_index, flags, ops->load, ctx);
    s->e[s->n - 1].ops = ops;
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
    return s->parent ? au_fontset_has_family(s->parent, family, len) : false;
}

void au_fontset_stats(const AuFontSet *s, AuFontSetStats *st)
{
    *st = s->stats;
    st->loaded = 0;
    st->loaded_bytes = 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->e[i].face) {
            st->loaded++;
            if (s->e[i].pager) {
                AuFontPagerStats ps;
                au_font_pager_stats(s->e[i].pager, &ps);
                st->loaded_bytes += ps.table_bytes + ps.block_bytes;
                st->paged++;
            } else {
                st->loaded_bytes += s->e[i].face->len;
            }
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
    uint32_t size = 0;
    if (e->ops && e->ops->size && e->ops->read && e->ops->size(e->ctx, &size) && size > AU_FONT_PAGED_MIN) {
        /* A large file: its outlines stay in the file. */
        e->pager = au_font_pager_new(e->ops->read, e->ctx, size, e->index);
        e->face = au_font_face_new_paged(e->pager);
        if (!e->face) {
            au_font_pager_free(e->pager);
            e->pager = NULL;
        }
    } else if (e->load && e->load(e->ctx, &data, &len)) {
        e->face = au_font_face_new(data, len, e->index);
    }
    if (!e->face) {
        e->failed = true;
    } else {
        s->stats.loads++;
        note_axes(e);
    }
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

/* The weight of an entry's range nearest to want (CSS Fonts 4: a range
 * that contains the desired weight matches it exactly). */
static int range_weight(const Entry *e, int want)
{
    if (want >= e->wmin && want <= e->wmax)
        return want;
    int lo = weight_key(want, e->wmin), hi = weight_key(want, e->wmax);
    return lo <= hi ? e->wmin : e->wmax;
}

/* Whether the entry can draw italic: an italic face, or an ital or slnt
 * axis. */
static bool entry_italic(const Entry *e)
{
    if (e->italic)
        return true;
    if (!e->var || !e->face)
        return false;
    return au_font_axis_find(e->face, AU_FONT_TAG('i', 't', 'a', 'l')) >= 0 ||
           au_font_axis_find(e->face, AU_FONT_TAG('s', 'l', 'n', 't')) >= 0;
}

/* The internal style a match carries. */
typedef struct FStyle { /* not "Style": a QuickDraw type */
    int weight;
    bool italic;
    float stretch, size;
    const AuFontVariation *settings;
    size_t nsettings;
} FStyle;

static FStyle plain_style(int weight, bool italic)
{
    FStyle st;
    memset(&st, 0, sizeof st);
    st.weight = weight;
    st.italic = italic;
    return st;
}

static void var_set(AuFontVariation *v, uint8_t *nv, uint32_t tag, float value)
{
    for (uint8_t i = 0; i < *nv; i++)
        if (v[i].tag == tag) {
            v[i].value = value;
            return;
        }
    if (*nv < INST_VARS) {
        v[*nv].tag = tag;
        v[*nv].value = value;
        (*nv)++;
    }
}

/* The face for a variable entry at the style's axis positions, with the
 * styles the axes cannot give synthesized. */
static AuFontFace *var_instance(Entry *e, const FStyle *st)
{
    AuFontFace *f = e->face;
    AuFontVariation v[INST_VARS];
    uint8_t nv = 0;
    int want = range_weight(e, st->weight);
    bool has_w = au_font_axis_find(f, AU_FONT_TAG('w', 'g', 'h', 't')) >= 0;
    bool has_i = au_font_axis_find(f, AU_FONT_TAG('i', 't', 'a', 'l')) >= 0;
    bool has_s = au_font_axis_find(f, AU_FONT_TAG('s', 'l', 'n', 't')) >= 0;
    if (has_w)
        var_set(v, &nv, AU_FONT_TAG('w', 'g', 'h', 't'), (float)want);
    if (st->stretch > 0)
        var_set(v, &nv, AU_FONT_TAG('w', 'd', 't', 'h'), st->stretch);
    if (st->italic && !e->italic) {
        if (has_i)
            var_set(v, &nv, AU_FONT_TAG('i', 't', 'a', 'l'), 1);
        else if (has_s)
            var_set(v, &nv, AU_FONT_TAG('s', 'l', 'n', 't'), -14); /* oblique 14deg, as CSS italic falls back */
    }
    if (st->size > 0)
        var_set(v, &nv, AU_FONT_TAG('o', 'p', 's', 'z'), st->size);
    for (size_t i = 0; i < st->nsettings; i++)
        var_set(v, &nv, st->settings[i].tag, st->settings[i].value);
    /* Drop positions on axes the font lacks or at their defaults, so
     * equivalent requests share one instance. */
    uint8_t k = 0;
    for (uint8_t i = 0; i < nv; i++) {
        int a = au_font_axis_find(f, v[i].tag);
        if (a < 0)
            continue;
        const AuFontAxis *ax = au_font_axis(f, a);
        float val = v[i].value < ax->min ? ax->min : v[i].value > ax->max ? ax->max : v[i].value;
        if (au_font_normalize(f, a, val) == 0)
            continue;
        v[k].tag = v[i].tag;
        v[k].value = val;
        k++;
    }
    nv = k;
    unsigned synth = 0;
    if (st->italic && !e->italic && !has_i && !has_s)
        synth |= AU_FONT_SYNTH_ITALIC;
    int got = has_w ? want : e->weight;
    if (st->weight >= 600 && got <= 500)
        synth |= AU_FONT_SYNTH_BOLD;
    if (!nv && !synth)
        return f;
    Inst *best = NULL;
    for (size_t i = 0; i < e->ninst; i++) {
        Inst *in = &e->inst[i];
        if (in->nv != nv || in->synth != synth)
            continue;
        bool same = true;
        for (uint8_t j = 0; j < nv && same; j++)
            same = in->v[j].tag == v[j].tag && in->v[j].value == v[j].value;
        if (same)
            return in->face;
        if (!best)
            best = in;
    }
    if (e->ninst >= MAX_INST)
        return best ? best->face : f; /* a page asking for very many distinct positions */
    if (!e->inst)
        e->inst = au_alloc(MAX_INST * sizeof *e->inst);
    Inst *in = &e->inst[e->ninst++];
    memcpy(in->v, v, sizeof v);
    in->nv = nv;
    in->synth = (uint8_t)synth;
    AuFontFace *iface = au_font_face_instance(f, v, nv);
    if (synth) {
        AuFontFace *sf = au_font_face_synthesize(iface, synth);
        au_font_face_free(iface);
        iface = sf;
    }
    if (e->italic)
        iface->italic = true;
    in->face = iface;
    return iface;
}

/* The matched face of e, or a variant: a variable face at the requested
 * positions; else one that synthesizes the italic or bold it lacks: italic
 * when an italic or oblique style was asked for and f is upright; bold
 * when 600 or more was asked for and f is 500 or lighter (Blink's
 * rule). */
static AuFontFace *synthesize(Entry *e, const FStyle *st)
{
    if (e->var)
        return var_instance(e, st);
    /* The entry's style, as registered, is what the face provides. */
    unsigned want = 0;
    if (st->italic && !e->italic)
        want |= AU_FONT_SYNTH_ITALIC;
    if (st->weight >= 600 && e->wmax <= 500)
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

static bool in_ranges(const Entry *e, uint32_t cp)
{
    if (!e->ranges)
        return true;
    for (size_t i = 0; i < e->nranges; i++)
        if (cp >= e->ranges[i * 2] && cp <= e->ranges[i * 2 + 1])
            return true;
    return false;
}

static bool entry_covers(const Entry *e, const uint32_t *cps, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!in_ranges(e, cps[i]))
            return false;
    return true;
}

/* The best face of family, loaded and with synthesized styles, or NULL.
 * With cps, only faces whose unicode-range covers them are considered;
 * without, faces covering the space (the face CSS takes metrics from) are
 * preferred. The set's own faces come before its parent's. */
static AuFontFace *match_family_cps(AuFontSet *s, const char *family, size_t flen, const FStyle *st,
                                    const uint32_t *cps, size_t ncps)
{
    for (;;) {
        Entry *best = NULL;
        int best_key = 0;
        for (size_t i = 0; i < s->n; i++) {
            Entry *e = &s->e[i];
            if (e->failed || !au_ieq(e->family, strlen(e->family), family, flen))
                continue;
            if (cps && !entry_covers(e, cps, ncps))
                continue;
            int key = weight_key(st->weight, range_weight(e, st->weight));
            if (st->italic != e->italic && !(st->italic && entry_italic(e)))
                key += 100000;
            if (!cps && !in_ranges(e, ' '))
                key += 200000;
            if (!best || key < best_key) {
                best = e;
                best_key = key;
            }
        }
        if (!best)
            break;
        bool was_var = best->var;
        if (load_entry(s, best)) {
            /* A lazily registered face turned out variable: its range
             * may make another entry the better match. */
            if (best->var && !was_var)
                continue;
            return synthesize(best, st);
        }
        /* It failed to load: it is marked, so the next pass skips it. */
    }
    return s->parent ? match_family_cps(s->parent, family, flen, st, cps, ncps) : NULL;
}

static AuFontFace *match_family(AuFontSet *s, const char *family, size_t flen, const FStyle *st)
{
    return match_family_cps(s, family, flen, st, NULL, 0);
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

/* The preference for generic slot g: this set's, else its parent's. */
static const char *generic_pref(const AuFontSet *s, int g)
{
    while (s && !s->generic[g][0])
        s = s->parent;
    return s ? s->generic[g] : "";
}

static void add_generic(AuFontSet *s, Cands *c, const AuFontAlias *g, int depth)
{
    if (g->generic >= 0) {
        const char *pref = generic_pref(s, g->generic);
        if (!pref[0] && g->generic == AU_GENERIC_SYSTEM_UI)
            pref = generic_pref(s, AU_GENERIC_SANS_SERIF);
        if (!pref[0] && (g->generic == AU_GENERIC_CURSIVE || g->generic == AU_GENERIC_FANTASY))
            pref = generic_pref(s, AU_GENERIC_SERIF);
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

static AuFontFace *any_face(AuFontSet *s, const FStyle *st)
{
    for (size_t i = 0; i < s->n; i++)
        if (!s->e[i].ranges && load_entry(s, &s->e[i]))
            return synthesize(&s->e[i], st);
    return s->parent ? any_face(s->parent, st) : NULL;
}

static AuFontFace *match_names(AuFontSet *s, const Name *names, size_t n, const FStyle *st)
{
    Cands c;
    build_cands(s, names, n, &c);
    for (size_t i = 0; i < c.n; i++) {
        AuFontFace *f = match_family(s, c.v[i].s, c.v[i].n, st);
        if (f)
            return f;
    }
    return any_face(s, st);
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
    FStyle st = plain_style(weight, italic);
    return match_names(s, names, k, &st);
}

static FStyle from_public(const AuFontStyle *p)
{
    FStyle st = plain_style(p->weight, p->italic);
    st.stretch = p->stretch;
    st.size = p->size;
    st.settings = p->settings;
    st.nsettings = p->nsettings;
    return st;
}

AuFontFace *au_fontset_match_style(AuFontSet *s, const char *families, const AuFontStyle *ps)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    const char *p = families ? families : "";
    while (k < MAX_NAMES && next_name(&p, &names[k]))
        k++;
    FStyle st = from_public(ps);
    return match_names(s, names, k, &st);
}

AuFontFace *au_fontset_match(AuFontSet *s, const char *families, int weight, bool italic)
{
    AuFontStyle st;
    memset(&st, 0, sizeof st);
    st.weight = weight;
    st.italic = italic;
    return au_fontset_match_style(s, families, &st);
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
    FStyle st;
    AuFontVariation settings[INST_VARS];
    AuFontFace *primary;
    bool primary_ranged; /* the primary family has unicode-range faces */
    size_t ncand;
    ListCand *cand;
    struct {
        uint32_t cp1; /* code point + 1, times 2 plus emoji */
        AuFontFace *face;
    } cache[LIST_SLOTS];
};

static bool family_ranged(const AuFontSet *s, const char *family, size_t flen);

static AuFontList *list_new(AuFontSet *s, const Name *names, size_t n, const FStyle *st)
{
    AuFontList *l = au_calloc(sizeof *l);
    l->set = s;
    l->st = *st;
    if (st->nsettings) {
        size_t k = st->nsettings < INST_VARS ? st->nsettings : INST_VARS;
        memcpy(l->settings, st->settings, k * sizeof *l->settings);
        l->st.settings = l->settings;
        l->st.nsettings = k;
    }
    Cands c;
    build_cands(s, names, n, &c);
    l->cand = au_calloc((c.n ? c.n : 1) * sizeof *l->cand);
    l->ncand = c.n;
    for (size_t i = 0; i < c.n; i++)
        copy_name(l->cand[i].name, sizeof l->cand[i].name, c.v[i].s, c.v[i].n);
    for (size_t i = 0; i < l->ncand && !l->primary; i++) {
        ListCand *lc = &l->cand[i];
        lc->face = match_family(s, lc->name, strlen(lc->name), &l->st);
        lc->state = lc->face ? 1 : -1;
        l->primary = lc->face;
        if (lc->face)
            l->primary_ranged = family_ranged(s, lc->name, strlen(lc->name));
    }
    if (!l->primary)
        l->primary = any_face(s, &l->st);
    return l;
}

static void split_names(const char *const *families, size_t n, Name *names, size_t *k)
{
    *k = 0;
    for (size_t i = 0; i < n && *k < MAX_NAMES; i++) {
        names[*k].s = families[i];
        names[*k].n = strlen(families[i]);
        names[*k].quoted = false;
        (*k)++;
    }
}

AuFontList *au_fontlist_new(AuFontSet *s, const char *const *families, size_t n, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k;
    split_names(families, n, names, &k);
    FStyle st = plain_style(weight, italic);
    return list_new(s, names, k, &st);
}

AuFontList *au_fontlist_new_style(AuFontSet *s, const char *const *families, size_t n, const AuFontStyle *ps)
{
    Name names[MAX_NAMES];
    size_t k;
    split_names(families, n, names, &k);
    FStyle st = from_public(ps);
    return list_new(s, names, k, &st);
}

AuFontList *au_fontlist_parse(AuFontSet *s, const char *families, int weight, bool italic)
{
    Name names[MAX_NAMES];
    size_t k = 0;
    const char *p = families ? families : "";
    while (k < MAX_NAMES && next_name(&p, &names[k]))
        k++;
    FStyle st = plain_style(weight, italic);
    return list_new(s, names, k, &st);
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

/* Whether f would draw cps[0] in colour. */
static bool draws_color(const AuFontFace *f, const uint32_t *cps)
{
    return f->color && au_colr_has(f, au_font_glyph(f, cps[0]));
}

/* The first face of a comma-separated fallback list that has cps; with
 * mono, faces that would draw it in colour are passed over (text
 * presentation). */
static AuFontFace *from_names_mode(AuFontSet *s, const char *names, const uint32_t *cps, size_t n, const FStyle *st,
                                   bool mono)
{
    const char *p = names;
    Name nm;
    while (next_name(&p, &nm)) {
        AuFontFace *f = match_family_cps(s, nm.s, nm.n, st, cps, n);
        if (f && has_all(f, cps, n) && !(mono && draws_color(f, cps)))
            return f;
    }
    return NULL;
}

static AuFontFace *from_names(AuFontSet *s, const char *names, const uint32_t *cps, size_t n, const FStyle *st)
{
    return from_names_mode(s, names, cps, n, st, false);
}

/* A loaded face of s (or its parents) that has cps. */
static AuFontFace *any_loaded(AuFontSet *s, const uint32_t *cps, size_t n, const FStyle *st)
{
    for (; s; s = s->parent)
        for (size_t i = 0; i < s->n; i++) {
            Entry *e = &s->e[i];
            if (e->face && entry_covers(e, cps, n) && has_all(e->face, cps, n))
                return synthesize(e, st);
        }
    return NULL;
}

/* The global stage. Emoji: the emoji fallback list only. Text: the text
 * fallback list, the emoji list, then any face already loaded (none is
 * loaded only to look). Optical size and variation settings stay with the
 * page's own families. */
static AuFontFace *global_face(AuFontSet *s, const uint32_t *cps, size_t n, const FStyle *full, bool emoji)
{
    FStyle st = plain_style(full->weight, full->italic);
    st.stretch = full->stretch;
    if (emoji)
        return from_names(s, au_font_emoji_fallbacks(), cps, n, &st);
    AuFontFace *f = from_names(s, au_font_text_fallbacks(), cps, n, &st);
    /* Text presentation: a monochrome emoji face before a colour one. */
    if (!f)
        f = from_names_mode(s, au_font_emoji_fallbacks(), cps, n, &st, true);
    if (!f)
        f = from_names(s, au_font_emoji_fallbacks(), cps, n, &st);
    if (!f)
        f = any_loaded(s, cps, n, &st);
    return f;
}

static AuFontFace *global_cached(AuFontSet *s, uint32_t cp, const FStyle *st, bool emoji)
{
    uint16_t key = (uint16_t)(((st->weight / 100) << 2) | (st->italic ? 2 : 0) | (emoji ? 1 : 0));
    uint32_t h = (cp * 2654435761u) ^ (key * 40503u);
    FbSlot *slot = &s->fb[(h >> 7) & (FB_SLOTS - 1)];
    if (slot->cp1 == cp + 1 && slot->key == key && st->stretch == 0) {
        s->stats.fallback_hits++;
        return slot->face;
    }
    s->stats.fallback_misses++;
    AuFontFace *f = global_face(s, &cp, 1, st, emoji);
    if (st->stretch == 0) {
        slot->cp1 = cp + 1;
        slot->key = key;
        slot->face = f;
    }
    return f;
}

/* Whether a face of the family is restricted by unicode-range. */
static bool family_ranged(const AuFontSet *s, const char *family, size_t flen)
{
    for (; s; s = s->parent)
        for (size_t i = 0; i < s->n; i++)
            if (s->e[i].ranges && au_ieq(s->e[i].family, strlen(s->e[i].family), family, flen))
                return true;
    return false;
}

/* The faces of the list itself, in order, that have cps. A family split
 * over several faces by unicode-range gives the face covering cps. With
 * color_only, only colour fonts count (a page naming a colour emoji font
 * for emoji). */
static AuFontFace *from_list_any(AuFontList *l, const uint32_t *cps, size_t n, bool color_only)
{
    for (size_t i = 0; i < l->ncand; i++) {
        ListCand *lc = &l->cand[i];
        if (lc->state == 0) {
            lc->face = match_family(l->set, lc->name, strlen(lc->name), &l->st);
            lc->state = lc->face ? 1 : -1;
        }
        if (lc->state <= 0)
            continue;
        AuFontFace *f = lc->face;
        if (color_only && !f->color)
            continue;
        size_t len = strlen(lc->name);
        if (family_ranged(l->set, lc->name, len))
            f = match_family_cps(l->set, lc->name, len, &l->st, cps, n);
        if (f && has_all(f, cps, n) && (!color_only || f->color))
            return f;
    }
    return NULL;
}

static AuFontFace *from_list(AuFontList *l, const uint32_t *cps, size_t n)
{
    return from_list_any(l, cps, n, false);
}

AuFontFace *au_fontlist_face_for_cluster(AuFontList *l, const uint32_t *cps, size_t n, bool emoji)
{
    if (!l || !l->primary || !n)
        return NULL;
    if (!emoji && !l->primary_ranged && has_all(l->primary, cps, n))
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
    if (emoji) {
        /* Emoji: a colour font the page names, then the emoji fallback
         * faces (colour first), then the page's other faces. */
        f = from_list_any(l, cps, n, true);
        if (!f)
            f = n == 1 ? global_cached(l->set, cps[0], &l->st, true) : global_face(l->set, cps, n, &l->st, true);
    }
    if (!f)
        f = from_list(l, cps, n);
    if (!f)
        f = n == 1 ? global_cached(l->set, cps[0], &l->st, false) : global_face(l->set, cps, n, &l->st, false);
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
