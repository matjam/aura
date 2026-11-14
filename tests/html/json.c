#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s, *end;
} P;

static void ws(P *p)
{
    while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
        p->s++;
}

static void put(char **b, size_t *len, size_t *cap, char c)
{
    if (*len + 2 > *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *b = realloc(*b, *cap);
    }
    (*b)[(*len)++] = c;
    (*b)[*len] = 0;
}

static void put_cp(char **b, size_t *len, size_t *cap, unsigned cp)
{
    if (cp < 0x80) {
        put(b, len, cap, (char)cp);
    } else if (cp < 0x800) {
        put(b, len, cap, (char)(0xC0 | (cp >> 6)));
        put(b, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put(b, len, cap, (char)(0xE0 | (cp >> 12)));
        put(b, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(b, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else {
        put(b, len, cap, (char)(0xF0 | (cp >> 18)));
        put(b, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put(b, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(b, len, cap, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    *out = v;
    return 1;
}

/* Decodes \uHHHH (with surrogate pairs) from s; other bytes copy through.
 * If json is true, the other JSON escapes are decoded too. */
static char *unescape(const char *s, size_t n, size_t *outlen, int json)
{
    char *b = NULL;
    size_t len = 0, cap = 0;
    put(&b, &len, &cap, 0);
    len = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            put(&b, &len, &cap, s[i]);
            continue;
        }
        char e = s[i + 1];
        unsigned cp;
        if (e == 'u' && i + 5 < n + 0 && hex4(s + i + 2, &cp)) {
            i += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo;
                if (hex4(s + i + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            put_cp(&b, &len, &cap, cp);
            continue;
        }
        if (!json) {
            put(&b, &len, &cap, s[i]);
            continue;
        }
        i++;
        switch (e) {
        case 'n': put(&b, &len, &cap, '\n'); break;
        case 't': put(&b, &len, &cap, '\t'); break;
        case 'r': put(&b, &len, &cap, '\r'); break;
        case 'b': put(&b, &len, &cap, '\b'); break;
        case 'f': put(&b, &len, &cap, '\f'); break;
        default: put(&b, &len, &cap, e); break;
        }
    }
    *outlen = len;
    return b;
}

char *json_unescape_u(const char *s, size_t len, size_t *outlen)
{
    return unescape(s, len, outlen, 0);
}

static JVal *value(P *p);

static JVal *newv(JType t)
{
    JVal *v = calloc(1, sizeof *v);
    v->type = t;
    return v;
}

static char *string(P *p, size_t *len)
{
    if (p->s >= p->end || *p->s != '"')
        return NULL;
    const char *start = ++p->s;
    while (p->s < p->end && *p->s != '"') {
        if (*p->s == '\\')
            p->s++;
        p->s++;
    }
    if (p->s >= p->end)
        return NULL;
    char *r = unescape(start, (size_t)(p->s - start), len, 1);
    p->s++;
    return r;
}

static JVal *value(P *p)
{
    ws(p);
    if (p->s >= p->end)
        return NULL;
    char c = *p->s;
    if (c == '"') {
        JVal *v = newv(J_STR);
        v->str = string(p, &v->len);
        if (!v->str) {
            free(v);
            return NULL;
        }
        return v;
    }
    if (c == '[' || c == '{') {
        int obj = c == '{';
        JVal *v = newv(obj ? J_OBJ : J_ARR);
        p->s++;
        size_t cap = 0;
        for (;;) {
            ws(p);
            if (p->s < p->end && *p->s == (obj ? '}' : ']')) {
                p->s++;
                return v;
            }
            if (v->n) {
                if (p->s >= p->end || *p->s != ',')
                    return json_free(v), NULL;
                p->s++;
                ws(p);
            }
            if (v->n == cap) {
                cap = cap ? cap * 2 : 8;
                v->items = realloc(v->items, cap * sizeof *v->items);
                if (obj) {
                    v->keys = realloc(v->keys, cap * sizeof *v->keys);
                    v->keylens = realloc(v->keylens, cap * sizeof *v->keylens);
                }
            }
            if (obj) {
                v->keys[v->n] = string(p, &v->keylens[v->n]);
                if (!v->keys[v->n])
                    return json_free(v), NULL;
                ws(p);
                if (p->s >= p->end || *p->s != ':') {
                    free(v->keys[v->n]);
                    return json_free(v), NULL;
                }
                p->s++;
            }
            JVal *item = value(p);
            if (!item) {
                if (obj)
                    free(v->keys[v->n]);
                return json_free(v), NULL;
            }
            v->items[v->n++] = item;
        }
    }
    if (!strncmp(p->s, "true", 4)) {
        p->s += 4;
        return newv(J_TRUE);
    }
    if (!strncmp(p->s, "false", 5)) {
        p->s += 5;
        return newv(J_FALSE);
    }
    if (!strncmp(p->s, "null", 4)) {
        p->s += 4;
        return newv(J_NULL);
    }
    char *e;
    double d = strtod(p->s, &e);
    if (e == p->s)
        return NULL;
    p->s = e;
    JVal *v = newv(J_NUM);
    v->num = d;
    return v;
}

JVal *json_parse(const char *s, size_t len)
{
    P p = {s, s + len};
    return value(&p);
}

void json_free(JVal *v)
{
    if (!v)
        return;
    for (size_t i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->keylens);
    free(v->str);
    free(v);
}

JVal *json_get(const JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}
