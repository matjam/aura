/* Script hints (script_hints.h). */
#include "script_hints.h"
#include "../js/hint_store.h"

#include <string.h>

typedef struct HintScript {
    uint32_t len, hash;
    uint32_t used; /* clock of the last get or add; 0: the slot is free */
    uint32_t *ofs; /* sorted, ascending, no repeats */
    uint32_t n, cap;
} HintScript;

struct AuScriptHints {
    HintScript s[AU_SCRIPT_HINTS_SCRIPTS];
    uint32_t clock;
    uint32_t total; /* offsets in all scripts */
    bool dirty;
    AuJsHintStore store;
};

static void slot_free(AuScriptHints *h, HintScript *s)
{
    h->total -= s->n;
    au_free(s->ofs);
    memset(s, 0, sizeof *s);
}

/* The least recently used script other than keep, or NULL. */
static HintScript *oldest(AuScriptHints *h, const HintScript *keep)
{
    HintScript *o = NULL;
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++) {
        HintScript *s = &h->s[i];
        if (s->used && s != keep && (!o || s->used < o->used))
            o = s;
    }
    return o;
}

static HintScript *find(AuScriptHints *h, uint32_t len, uint32_t hash)
{
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++)
        if (h->s[i].used && h->s[i].len == len && h->s[i].hash == hash)
            return &h->s[i];
    return NULL;
}

/* A slot for a new script: a free one, else the least recently used. */
static HintScript *take_slot(AuScriptHints *h, uint32_t len, uint32_t hash)
{
    HintScript *s = NULL;
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS && !s; i++)
        if (!h->s[i].used)
            s = &h->s[i];
    if (!s)
        s = oldest(h, NULL);
    slot_free(h, s);
    s->len = len;
    s->hash = hash;
    s->used = ++h->clock;
    return s;
}

/* Inserts ofs into s; false when it was there or s is full. Past the
 * total, the least recently used other scripts go. */
static bool insert(AuScriptHints *h, HintScript *s, uint32_t ofs)
{
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->ofs[mid] == ofs)
            return false;
        if (s->ofs[mid] < ofs)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (s->n >= AU_SCRIPT_HINTS_MAX)
        return false;
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 64;
        if (cap > AU_SCRIPT_HINTS_MAX)
            cap = AU_SCRIPT_HINTS_MAX;
        s->ofs = au_realloc(s->ofs, (size_t)cap * sizeof *s->ofs);
        s->cap = cap;
    }
    memmove(s->ofs + lo + 1, s->ofs + lo, (size_t)(s->n - lo) * sizeof *s->ofs);
    s->ofs[lo] = ofs;
    s->n++;
    h->total++;
    while (h->total > AU_SCRIPT_HINTS_TOTAL) {
        HintScript *o = oldest(h, s);
        if (!o)
            break;
        slot_free(h, o);
    }
    return true;
}

static const uint32_t *store_get(void *ctx, uint32_t len, uint32_t hash, size_t *count)
{
    AuScriptHints *h = ctx;
    HintScript *s = find(h, len, hash);
    *count = 0;
    if (!s || !s->n)
        return NULL;
    s->used = ++h->clock;
    *count = s->n;
    return s->ofs;
}

static void store_add(void *ctx, uint32_t len, uint32_t hash, uint32_t ofs)
{
    AuScriptHints *h = ctx;
    if (ofs >= len)
        return;
    HintScript *s = find(h, len, hash);
    if (!s)
        s = take_slot(h, len, hash);
    s->used = ++h->clock;
    if (insert(h, s, ofs))
        h->dirty = true;
}

AuScriptHints *au_script_hints_new(void)
{
    AuScriptHints *h = au_calloc(sizeof *h);
    h->store.ctx = h;
    h->store.get = store_get;
    h->store.add = store_add;
    return h;
}

void au_script_hints_free(AuScriptHints *h)
{
    if (!h)
        return;
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++)
        slot_free(h, &h->s[i]);
    au_free(h);
}

void au_script_hints_clear(AuScriptHints *h)
{
    bool any = false;
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++) {
        any |= h->s[i].used != 0;
        slot_free(h, &h->s[i]);
    }
    if (any)
        h->dirty = true;
}

const AuJsHintStore *au_script_hints_store(AuScriptHints *h)
{
    return &h->store;
}

size_t au_script_hints_count(const AuScriptHints *h)
{
    size_t n = 0;
    for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++)
        n += h->s[i].used && h->s[i].n;
    return n;
}

bool au_script_hints_dirty(const AuScriptHints *h)
{
    return h->dirty;
}

void au_script_hints_clear_dirty(AuScriptHints *h)
{
    h->dirty = false;
}

/* ---- File ---- */

#define HEADER "# Aura script hints 1\n"

void au_script_hints_serialize(const AuScriptHints *h, AuBuf *out)
{
    au_buf_puts(out, HEADER);
    /* the most recently used first, so a load keeps the order */
    uint32_t below = UINT32_MAX;
    for (;;) {
        const HintScript *s = NULL;
        for (size_t i = 0; i < AU_SCRIPT_HINTS_SCRIPTS; i++) {
            const HintScript *c = &h->s[i];
            if (c->used && c->n && c->used < below && (!s || c->used > s->used))
                s = c;
        }
        if (!s)
            break;
        below = s->used;
        au_buf_printf(out, "%lx %lx", (unsigned long)s->len, (unsigned long)s->hash);
        uint32_t prev = 0;
        for (uint32_t k = 0; k < s->n; k++) {
            au_buf_printf(out, " %lx", (unsigned long)(s->ofs[k] - prev));
            prev = s->ofs[k];
        }
        au_buf_putc(out, '\n');
    }
}

/* A hexadecimal number of at most 8 digits at *p, before end. */
static bool hex(const char **p, const char *end, uint32_t *out)
{
    const char *q = *p;
    uint32_t v = 0;
    int digits = 0;
    for (; q < end; q++) {
        char c = *q;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            break;
        if (++digits > 8)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    if (!digits)
        return false;
    *p = q;
    *out = v;
    return true;
}

static void parse_line(AuScriptHints *h, const char *p, const char *end, uint32_t used)
{
    uint32_t len, hash;
    if (!hex(&p, end, &len) || p >= end || *p++ != ' ' || !hex(&p, end, &hash) || find(h, len, hash))
        return;
    HintScript *s = take_slot(h, len, hash);
    s->used = used;
    uint32_t prev = 0;
    while (p < end && *p == ' ') {
        p++;
        uint32_t d;
        const char *q = p;
        /* offsets ascend: a difference of 0 after the first, or one past
         * the script's end, is damage */
        if (!hex(&q, end, &d) || (s->n && !d) || d >= len - prev || s->n >= AU_SCRIPT_HINTS_MAX)
            break;
        /* past the total, the lines after (used less recently) go */
        if (h->total >= AU_SCRIPT_HINTS_TOTAL)
            break;
        p = q;
        prev += d;
        insert(h, s, prev);
    }
    if (p != end || !s->n)
        slot_free(h, s);
}

bool au_script_hints_parse(AuScriptHints *h, const char *data, size_t len)
{
    size_t hl = sizeof HEADER - 1;
    if (len < hl || memcmp(data, HEADER, hl))
        return false;
    const char *p = data + hl, *end = data + len;
    /* the first line is the most recently used: number down from the
     * table's size */
    uint32_t used = AU_SCRIPT_HINTS_SCRIPTS + 1;
    while (p < end && used > 1) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        parse_line(h, p, e, --used);
        p = nl ? nl + 1 : end;
    }
    if (h->clock < AU_SCRIPT_HINTS_SCRIPTS + 1)
        h->clock = AU_SCRIPT_HINTS_SCRIPTS + 1;
    return true;
}
