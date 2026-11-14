/* Font sets: the faces the platform has, and CSS font matching. */
#include "font.h"

#include <string.h>

typedef struct Entry {
    char family[64];
    int weight;
    bool italic;
    int index; /* face index in its file */
    AuFontFace *face;
    const void *data; /* when registered with data */
    size_t len;
    AuFontLoader load;
    void *ctx;
    bool failed;
} Entry;

struct AuFontSet {
    Entry *e;
    size_t n, cap;
    char generic[AU_GENERIC_COUNT][64];
};

AuFontSet *au_fontset_new(void)
{
    return au_calloc(sizeof(AuFontSet));
}

void au_fontset_free(AuFontSet *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++)
        au_font_face_free(s->e[i].face);
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

void au_fontset_add_lazy(AuFontSet *s, const char *family, int weight, bool italic, int face_index,
                         AuFontLoader load, void *ctx)
{
    Entry *e = new_entry(s);
    copy_name(e->family, sizeof e->family, family, strlen(family));
    e->weight = weight;
    e->italic = italic;
    e->index = face_index;
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

static bool load_entry(Entry *e)
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

/* The best loaded entry of family, or NULL. */
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
        if (load_entry(best))
            return best->face;
        /* It failed to load: it is marked, so the next pass skips it. */
    }
}

static int generic_of(const char *name, size_t n)
{
    static const char *names[AU_GENERIC_COUNT] = {"serif", "sans-serif", "monospace", "system-ui", "cursive",
                                                  "fantasy"};
    for (int i = 0; i < AU_GENERIC_COUNT; i++)
        if (au_ieq(name, n, names[i], strlen(names[i])))
            return i;
    if (au_ieq(name, n, "ui-serif", 8))
        return AU_GENERIC_SERIF;
    if (au_ieq(name, n, "ui-sans-serif", 13) || au_ieq(name, n, "-apple-system", 13))
        return AU_GENERIC_SANS_SERIF;
    if (au_ieq(name, n, "ui-monospace", 12))
        return AU_GENERIC_MONOSPACE;
    return -1;
}

static AuFontFace *match_one(AuFontSet *s, const char *name, size_t n, bool quoted, int weight, bool italic)
{
    int g = quoted ? -1 : generic_of(name, n);
    if (g >= 0) {
        const char *fam = s->generic[g];
        if (!fam[0] && g == AU_GENERIC_SYSTEM_UI)
            fam = s->generic[AU_GENERIC_SANS_SERIF];
        if (!fam[0] && (g == AU_GENERIC_CURSIVE || g == AU_GENERIC_FANTASY))
            fam = s->generic[AU_GENERIC_SERIF];
        return fam[0] ? match_family(s, fam, strlen(fam), weight, italic) : NULL;
    }
    return match_family(s, name, n, weight, italic);
}

static AuFontFace *fallback(AuFontSet *s, int weight, bool italic)
{
    const char *sans = s->generic[AU_GENERIC_SANS_SERIF];
    AuFontFace *f = sans[0] ? match_family(s, sans, strlen(sans), weight, italic) : NULL;
    for (size_t i = 0; !f && i < s->n; i++)
        if (load_entry(&s->e[i]))
            f = s->e[i].face;
    return f;
}

AuFontFace *au_fontset_match_list(AuFontSet *s, const char *const *families, size_t n, int weight,
                                  bool italic)
{
    for (size_t i = 0; i < n; i++) {
        AuFontFace *f = match_one(s, families[i], strlen(families[i]), false, weight, italic);
        if (f)
            return f;
    }
    return fallback(s, weight, italic);
}

AuFontFace *au_fontset_match(AuFontSet *s, const char *families, int weight, bool italic)
{
    const char *p = families ? families : "";
    while (*p) {
        while (*p == ',' || au_is_space((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start, *end;
        bool quoted = false;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            start = p;
            while (*p && *p != q)
                p++;
            end = p;
            if (*p)
                p++;
            quoted = true;
            while (*p && *p != ',')
                p++;
        } else {
            start = p;
            while (*p && *p != ',')
                p++;
            end = p;
            /* Unquoted names: runs of identifiers separated by spaces;
             * collapse to the trimmed text. */
            while (end > start && au_is_space((unsigned char)end[-1]))
                end--;
        }
        AuFontFace *f = match_one(s, start, (size_t)(end - start), quoted, weight, italic);
        if (f)
            return f;
    }
    return fallback(s, weight, italic);
}
