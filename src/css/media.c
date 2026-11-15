/* Media queries (Media Queries Level 4/5: types, not/only/and/or, plain,
 * min-/max- and range features) and @supports conditions.
 *
 * A media query list compiles into a small tree of nodes evaluated
 * against an AuCssDevice. An invalid query in a list becomes "not all";
 * <general-enclosed> evaluates to false. Lengths in queries use the initial
 * font size (16px) for em and rem. */
#include "css_int.h"

#include <math.h>

enum { MQ_QUERY_LIST = 1, MQ_AND, MQ_OR, MQ_NOT, MQ_FEATURE, MQ_TYPE, MQ_CONST };
enum { OP_NONE = 0, OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE };
enum { VT_NONE, VT_LENGTH, VT_NUMBER, VT_RATIO, VT_RES, VT_IDENT };

typedef struct MqNode {
    uint8_t kind;
    uint8_t feature;
    uint8_t op[2];  /* feature op value, for up to two constraints */
    uint8_t vt[2];
    uint8_t unit[2];
    uint16_t kw[2];
    float v[2];
    int32_t child, next; /* indices, -1 for none */
    bool value;          /* MQ_CONST, MQ_TYPE result for unknown types */
} MqNode;

struct AuCssMediaList {
    MqNode *n;
    uint32_t count, cap;
    char *text;
};

/* ---- Features ---- */

enum { FT_RANGE_LEN, FT_RANGE_RATIO, FT_RANGE_RES, FT_RANGE_INT, FT_DISCRETE };

static const struct {
    const char *name;
    uint8_t type;
} features[] = {
    {"width", FT_RANGE_LEN},              /* 0 */
    {"height", FT_RANGE_LEN},             /* 1 */
    {"device-width", FT_RANGE_LEN},       /* 2 */
    {"device-height", FT_RANGE_LEN},      /* 3 */
    {"aspect-ratio", FT_RANGE_RATIO},     /* 4 */
    {"device-aspect-ratio", FT_RANGE_RATIO}, /* 5 */
    {"resolution", FT_RANGE_RES},         /* 6 */
    {"color", FT_RANGE_INT},              /* 7 */
    {"color-index", FT_RANGE_INT},        /* 8 */
    {"monochrome", FT_RANGE_INT},         /* 9 */
    {"grid", FT_DISCRETE},                /* 10 */
    {"orientation", FT_DISCRETE},         /* 11 */
    {"scan", FT_DISCRETE},                /* 12 */
    {"hover", FT_DISCRETE},               /* 13 */
    {"any-hover", FT_DISCRETE},           /* 14 */
    {"pointer", FT_DISCRETE},             /* 15 */
    {"any-pointer", FT_DISCRETE},         /* 16 */
    {"prefers-color-scheme", FT_DISCRETE}, /* 17 */
    {"prefers-reduced-motion", FT_DISCRETE}, /* 18 */
    {"prefers-reduced-transparency", FT_DISCRETE}, /* 19 */
    {"prefers-contrast", FT_DISCRETE},    /* 20 */
    {"forced-colors", FT_DISCRETE},       /* 21 */
    {"inverted-colors", FT_DISCRETE},     /* 22 */
    {"display-mode", FT_DISCRETE},        /* 23 */
    {"scripting", FT_DISCRETE},           /* 24 */
    {"update", FT_DISCRETE},              /* 25 */
    {"dynamic-range", FT_DISCRETE},       /* 26 */
    {"video-dynamic-range", FT_DISCRETE}, /* 27 */
    {"color-gamut", FT_DISCRETE},         /* 28 */
    {"overflow-block", FT_DISCRETE},      /* 29 */
    {"overflow-inline", FT_DISCRETE},     /* 30 */
    {"-webkit-device-pixel-ratio", FT_RANGE_RES}, /* 31: a number of dppx */
    {"prefers-reduced-data", FT_DISCRETE}, /* 32 */
};

static int feature_lookup(const char *s, size_t n, int *prefix)
{
    char buf[48];
    if (!css_lower(s, n, buf, sizeof buf))
        return -1;
    const char *name = buf;
    *prefix = 0;
    if (strncmp(buf, "min-", 4) == 0) {
        *prefix = OP_GE;
        name += 4;
    } else if (strncmp(buf, "max-", 4) == 0) {
        *prefix = OP_LE;
        name += 4;
    } else if (strncmp(buf, "-webkit-min-", 12) == 0) {
        *prefix = OP_GE;
        name = "-webkit-device-pixel-ratio";
        if (strcmp(buf + 12, "device-pixel-ratio") != 0)
            return -1;
    } else if (strncmp(buf, "-webkit-max-", 12) == 0) {
        *prefix = OP_LE;
        name = "-webkit-device-pixel-ratio";
        if (strcmp(buf + 12, "device-pixel-ratio") != 0)
            return -1;
    }
    for (size_t i = 0; i < CSS_ARRAY_LEN(features); i++)
        if (strcmp(features[i].name, name) == 0) {
            if (*prefix && features[i].type == FT_DISCRETE)
                return -1;
            return (int)i;
        }
    return -1;
}

/* ---- Parsing ---- */

typedef struct {
    const CssTokens *tk;
    AuCssMediaList *m;
    int depth;
} MqP;

static int32_t new_node(MqP *p, int kind)
{
    MqNode *n = CSS_PUSH(p->m->n, p->m->count, p->m->cap);
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    n->child = n->next = -1;
    return (int32_t)(p->m->count - 1);
}

static uint32_t sk(const CssTokens *tk, uint32_t i, uint32_t b)
{
    while (i < b && tk->t[i].type == TK_WS)
        i++;
    return i;
}

static bool ident_is(const CssTokens *tk, uint32_t i, const char *s)
{
    const CssToken *t = &tk->t[i];
    return t->type == TK_IDENT && au_ieq(css_tok_str(tk, t), t->len, s, strlen(s));
}

/* Parses a feature value at *i: number [/ number], dimension, ident. */
static bool mq_value(MqP *p, uint32_t *i, uint32_t b, uint8_t *vt, float *v, uint8_t *unit, uint16_t *kw)
{
    const CssTokens *tk = p->tk;
    uint32_t k = sk(tk, *i, b);
    if (k >= b)
        return false;
    const CssToken *t = &tk->t[k];
    *unit = 0;
    *kw = 0;
    if (t->type == TK_NUMBER) {
        float a = t->num;
        uint32_t q = sk(tk, k + 1, b);
        if (q < b && tk->t[q].type == TK_DELIM && tk->t[q].num == '/') {
            q = sk(tk, q + 1, b);
            if (q >= b || tk->t[q].type != TK_NUMBER)
                return false;
            *vt = VT_RATIO;
            *v = tk->t[q].num != 0 ? a / tk->t[q].num : INFINITY;
            *i = q + 1;
            return true;
        }
        *vt = VT_NUMBER;
        *v = a;
        *i = k + 1;
        return true;
    }
    if (t->type == TK_DIMENSION) {
        float f;
        int c;
        if (au_unit_is_length(t->unit)) {
            *vt = VT_LENGTH;
            *v = t->num;
            *unit = (uint8_t)t->unit;
        } else if (css_unit_canonical(t->unit, &f, &c) && c == AU_UNIT_DPPX) {
            *vt = VT_RES;
            *v = t->num * f;
        } else {
            return false;
        }
        *i = k + 1;
        return true;
    }
    if (t->type == TK_IDENT) {
        *vt = VT_IDENT;
        *kw = t->kw;
        if (!*kw)
            *kw = 0xFFFF; /* unknown ident: matches nothing */
        *i = k + 1;
        return true;
    }
    if (t->type == TK_FUNCTION && (t->kw == AU_K_calc || t->kw == AU_K_min || t->kw == AU_K_max || t->kw == AU_K_clamp)) {
        CssArena a = {0};
        CssParseCtx cx = {&a, false, false, NULL};
        uint32_t q = k;
        AuCssValue *val = css_parse_component(&cx, tk, &q, b);
        bool ok = false;
        if (val && (val->type == AU_CSSV_NUMBER || val->type == AU_CSSV_DIMENSION)) {
            ok = true;
            if (val->type == AU_CSSV_NUMBER) {
                *vt = VT_NUMBER;
                *v = val->u.num;
            } else if (au_unit_is_length(val->unit)) {
                *vt = VT_LENGTH;
                *v = val->u.num;
                *unit = val->unit;
            } else {
                ok = false;
            }
            *i = q;
        }
        css_arena_free(&a);
        return ok;
    }
    return false;
}

static int parse_op(const CssTokens *tk, uint32_t *i, uint32_t b)
{
    uint32_t k = sk(tk, *i, b);
    if (k >= b || tk->t[k].type != TK_DELIM)
        return 0;
    int c = (int)tk->t[k].num;
    bool eq = k + 1 < b && tk->t[k + 1].type == TK_DELIM && tk->t[k + 1].num == '=';
    int op = 0;
    if (c == '=')
        op = OP_EQ;
    else if (c == '<')
        op = eq ? OP_LE : OP_LT;
    else if (c == '>')
        op = eq ? OP_GE : OP_GT;
    else
        return 0;
    *i = k + 1 + ((c != '=' && eq) ? 1 : 0);
    return op;
}

static int flip(int op)
{
    switch (op) {
    case OP_LT: return OP_GT;
    case OP_LE: return OP_GE;
    case OP_GT: return OP_LT;
    case OP_GE: return OP_LE;
    default: return op;
    }
}

/* Contents of ( ... ) as a feature; returns a node or -1. */
static int32_t mq_feature(MqP *p, uint32_t a, uint32_t b)
{
    const CssTokens *tk = p->tk;
    uint32_t i = sk(tk, a, b);
    if (i >= b)
        return -1;
    int prefix;
    if (tk->t[i].type == TK_IDENT) {
        int f = feature_lookup(css_tok_str(tk, &tk->t[i]), tk->t[i].len, &prefix);
        uint32_t k = sk(tk, i + 1, b);
        if (k >= b) {
            /* boolean context */
            if (f < 0 || prefix)
                return -1;
            int32_t n = new_node(p, MQ_FEATURE);
            p->m->n[n].feature = (uint8_t)f;
            return n;
        }
        if (tk->t[k].type == TK_COLON) {
            if (f < 0)
                return -1;
            k++;
            int32_t n = new_node(p, MQ_FEATURE);
            MqNode *nd = &p->m->n[n];
            nd->feature = (uint8_t)f;
            nd->op[0] = (uint8_t)(prefix ? prefix : OP_EQ);
            if (!mq_value(p, &k, b, &nd->vt[0], &nd->v[0], &nd->unit[0], &nd->kw[0]))
                return -1;
            nd = &p->m->n[n];
            if (sk(tk, k, b) != b)
                return -1;
            return n;
        }
        /* range: name op value */
        if (prefix || f < 0)
            return -1;
        int op = parse_op(tk, &k, b);
        if (!op)
            return -1;
        int32_t n = new_node(p, MQ_FEATURE);
        MqNode *nd = &p->m->n[n];
        nd->feature = (uint8_t)f;
        nd->op[0] = (uint8_t)op;
        if (!mq_value(p, &k, b, &nd->vt[0], &nd->v[0], &nd->unit[0], &nd->kw[0]))
            return -1;
        return sk(tk, k, b) == b ? n : -1;
    }
    /* value op name [op value] */
    uint8_t vt;
    float v;
    uint8_t unit;
    uint16_t kw;
    if (!mq_value(p, &i, b, &vt, &v, &unit, &kw))
        return -1;
    int op1 = parse_op(tk, &i, b);
    if (!op1)
        return -1;
    i = sk(tk, i, b);
    if (i >= b || tk->t[i].type != TK_IDENT)
        return -1;
    int f = feature_lookup(css_tok_str(tk, &tk->t[i]), tk->t[i].len, &prefix);
    if (f < 0 || prefix || features[f].type == FT_DISCRETE)
        return -1;
    i++;
    int32_t n = new_node(p, MQ_FEATURE);
    MqNode *nd = &p->m->n[n];
    nd->feature = (uint8_t)f;
    nd->op[0] = (uint8_t)flip(op1);
    nd->vt[0] = vt;
    nd->v[0] = v;
    nd->unit[0] = unit;
    nd->kw[0] = kw;
    uint32_t k = sk(tk, i, b);
    if (k < b) {
        int op2 = parse_op(tk, &k, b);
        if (!op2 || (op1 == OP_EQ) || ((op1 == OP_LT || op1 == OP_LE) != (op2 == OP_LT || op2 == OP_LE)))
            return -1;
        if (!mq_value(p, &k, b, &vt, &v, &unit, &kw))
            return -1;
        nd = &p->m->n[n];
        nd->op[1] = (uint8_t)op2;
        nd->vt[1] = vt;
        nd->v[1] = v;
        nd->unit[1] = unit;
        nd->kw[1] = kw;
        if (sk(tk, k, b) != b)
            return -1;
    }
    return n;
}

static int32_t mq_condition(MqP *p, uint32_t *i, uint32_t b, bool allow_or);

/* <media-in-parens> at *i. */
static int32_t mq_in_parens(MqP *p, uint32_t *i, uint32_t b)
{
    const CssTokens *tk = p->tk;
    uint32_t k = sk(tk, *i, b);
    if (k >= b)
        return -1;
    const CssToken *t = &tk->t[k];
    if (t->type == TK_FUNCTION) {
        /* general-enclosed: false */
        *i = t->len;
        int32_t n = new_node(p, MQ_CONST);
        p->m->n[n].value = false;
        return n;
    }
    if (t->type != TK_LPAREN)
        return -1;
    uint32_t end = t->len;
    uint32_t s = k + 1, e = (end > s && tk->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
    *i = end;
    if (++p->depth > 32) {
        p->depth--;
        return -1;
    }
    /* a nested condition if it starts with ( or not */
    uint32_t q = sk(tk, s, e);
    int32_t r = -1;
    if (q < e && (tk->t[q].type == TK_LPAREN || ident_is(tk, q, "not") || tk->t[q].type == TK_FUNCTION)) {
        uint32_t qq = q;
        r = mq_condition(p, &qq, e, true);
        if (r >= 0 && sk(tk, qq, e) != e)
            r = -1;
    }
    if (r < 0)
        r = mq_feature(p, s, e);
    if (r < 0) {
        /* general-enclosed */
        r = new_node(p, MQ_CONST);
        p->m->n[r].value = false;
    }
    p->depth--;
    return r;
}

static int32_t mq_condition(MqP *p, uint32_t *i, uint32_t b, bool allow_or)
{
    const CssTokens *tk = p->tk;
    uint32_t k = sk(tk, *i, b);
    if (k < b && ident_is(tk, k, "not")) {
        k++;
        int32_t c = mq_in_parens(p, &k, b);
        if (c < 0)
            return -1;
        int32_t n = new_node(p, MQ_NOT);
        p->m->n[n].child = c;
        *i = k;
        return n;
    }
    int32_t first = mq_in_parens(p, &k, b);
    if (first < 0)
        return -1;
    int kind = 0;
    int32_t last = first;
    for (;;) {
        uint32_t q = sk(tk, k, b);
        if (q >= b)
            break;
        int this_kind = ident_is(tk, q, "and") ? MQ_AND : ident_is(tk, q, "or") ? MQ_OR : 0;
        if (!this_kind || (kind && this_kind != kind) || (this_kind == MQ_OR && !allow_or))
            break;
        kind = this_kind;
        q++;
        int32_t c = mq_in_parens(p, &q, b);
        if (c < 0)
            return -1;
        p->m->n[last].next = c;
        last = c;
        k = q;
    }
    *i = k;
    if (!kind)
        return first;
    int32_t n = new_node(p, kind);
    p->m->n[n].child = first;
    return n;
}

/* One media query in [a, b); returns its node or -1 if invalid. */
static int32_t mq_query(MqP *p, uint32_t a, uint32_t b)
{
    const CssTokens *tk = p->tk;
    uint32_t i = sk(tk, a, b);
    if (i >= b)
        return -1;
    if (tk->t[i].type == TK_LPAREN || (ident_is(tk, i, "not") && sk(tk, i + 1, b) < b &&
                                       tk->t[sk(tk, i + 1, b)].type == TK_LPAREN)) {
        int32_t c = mq_condition(p, &i, b, true);
        return (c >= 0 && sk(tk, i, b) == b) ? c : -1;
    }
    bool neg = false;
    if (ident_is(tk, i, "not")) {
        neg = true;
        i = sk(tk, i + 1, b);
    } else if (ident_is(tk, i, "only")) {
        i = sk(tk, i + 1, b);
    }
    if (i >= b || tk->t[i].type != TK_IDENT)
        return -1;
    uint16_t kw = tk->t[i].kw;
    if (kw == AU_K_and_ || kw == AU_K_or_ || kw == AU_K_not_ || kw == AU_K_only || kw == AU_K_layer)
        return -1;
    int32_t type = new_node(p, MQ_TYPE);
    p->m->n[type].kw[0] = kw ? kw : 0xFFFF;
    i++;
    int32_t cond = -1;
    uint32_t q = sk(tk, i, b);
    if (q < b) {
        if (!ident_is(tk, q, "and"))
            return -1;
        q++;
        cond = mq_condition(p, &q, b, false);
        if (cond < 0 || sk(tk, q, b) != b)
            return -1;
    }
    int32_t r = type;
    if (cond >= 0) {
        r = new_node(p, MQ_AND);
        p->m->n[r].child = type;
        p->m->n[type].next = cond;
    }
    if (neg) {
        int32_t n = new_node(p, MQ_NOT);
        p->m->n[n].child = r;
        r = n;
    }
    return r;
}

AuCssMediaList *css_media_parse(const CssTokens *tk, uint32_t a, uint32_t b)
{
    AuCssMediaList *m = au_calloc(sizeof *m);
    MqP p = {tk, m, 0};
    int32_t root = new_node(&p, MQ_QUERY_LIST);
    int32_t last = -1;
    uint32_t start = a;
    bool any = false;
    for (uint32_t i = a; i <= b;) {
        if (i == b || tk->t[i].type == TK_COMMA) {
            if (sk(tk, start, i) < i || i < b || any) {
                int32_t q = mq_query(&p, start, i);
                if (q < 0) {
                    q = new_node(&p, MQ_CONST); /* not all */
                    m->n[q].value = false;
                }
                if (last < 0)
                    m->n[root].child = q;
                else
                    m->n[last].next = q;
                last = q;
                any = true;
            }
            if (i == b)
                break;
            i++;
            start = i;
            continue;
        }
        i = (tk->t[i].type == TK_FUNCTION || tk->t[i].type == TK_LPAREN || tk->t[i].type == TK_LBRACKET ||
             tk->t[i].type == TK_LBRACE) ? tk->t[i].len : i + 1;
    }
    AuBuf t;
    au_buf_init(&t);
    uint32_t s = sk(tk, a, b), e = b;
    while (e > s && tk->t[e - 1].type == TK_WS)
        e--;
    css_tokens_serialize(tk, s, e, &t);
    m->text = au_buf_take(&t, NULL);
    return m;
}

AuCssMediaList *css_media_parse_text(const char *text, size_t len)
{
    au_css_init();
    CssTokens tk = {0};
    css_tokenize(text, len, &tk);
    AuCssMediaList *m = css_media_parse(&tk, 0, tk.n);
    css_tokens_free(&tk);
    return m;
}

void css_media_free(AuCssMediaList *m)
{
    if (!m)
        return;
    au_free(m->n);
    au_free(m->text);
    au_free(m);
}

void css_media_serialize(const AuCssMediaList *m, AuBuf *out) { au_buf_puts(out, m->text ? m->text : ""); }

/* ---- Evaluation ---- */

static float len_px(float v, int unit, const AuCssDevice *d)
{
    float f;
    int c;
    if (css_unit_canonical(unit, &f, &c))
        return v * f;
    switch (unit) {
    case AU_UNIT_EM: case AU_UNIT_REM: case AU_UNIT_LH: case AU_UNIT_RLH: case AU_UNIT_IC: case AU_UNIT_RIC:
        return v * 16;
    case AU_UNIT_EX: case AU_UNIT_REX: case AU_UNIT_CH: case AU_UNIT_RCH: case AU_UNIT_CAP: case AU_UNIT_RCAP:
        return v * 8;
    case AU_UNIT_VW: case AU_UNIT_SVW: case AU_UNIT_LVW: case AU_UNIT_DVW: case AU_UNIT_VI: case AU_UNIT_SVI:
    case AU_UNIT_LVI: case AU_UNIT_DVI: case AU_UNIT_CQW: case AU_UNIT_CQI:
        return v * d->width / 100;
    case AU_UNIT_VH: case AU_UNIT_SVH: case AU_UNIT_LVH: case AU_UNIT_DVH: case AU_UNIT_VB: case AU_UNIT_SVB:
    case AU_UNIT_LVB: case AU_UNIT_DVB: case AU_UNIT_CQH: case AU_UNIT_CQB:
        return v * d->height / 100;
    case AU_UNIT_VMIN: case AU_UNIT_SVMIN: case AU_UNIT_LVMIN: case AU_UNIT_DVMIN: case AU_UNIT_CQMIN:
        return v * (d->width < d->height ? d->width : d->height) / 100;
    case AU_UNIT_VMAX: case AU_UNIT_SVMAX: case AU_UNIT_LVMAX: case AU_UNIT_DVMAX: case AU_UNIT_CQMAX:
        return v * (d->width > d->height ? d->width : d->height) / 100;
    default:
        return v;
    }
}

static bool cmp(float a, int op, float b)
{
    const float eps = 1e-4f;
    switch (op) {
    case OP_EQ: return a > b - eps && a < b + eps;
    case OP_LT: return a < b;
    case OP_LE: return a <= b + eps;
    case OP_GT: return a > b;
    case OP_GE: return a >= b - eps;
    default: return false;
    }
}

static bool eval_feature(const MqNode *n, const AuCssDevice *d)
{
    int f = n->feature;
    int type = features[f].type;
    float actual = 0;
    uint16_t akw = 0;
    switch (f) {
    case 0: actual = d->width; break;
    case 1: actual = d->height; break;
    case 2: actual = d->device_width; break;
    case 3: actual = d->device_height; break;
    case 4: actual = d->height > 0 ? d->width / d->height : 0; break;
    case 5: actual = d->device_height > 0 ? d->device_width / d->device_height : 0; break;
    case 6: case 31: actual = d->resolution; break;
    case 7: actual = d->color_bits; break;
    case 8: actual = 0; break;
    case 9: actual = d->monochrome_bits; break;
    case 10: actual = d->grid; akw = 0; break;
    case 11: akw = d->height >= d->width ? AU_K_portrait : AU_K_landscape; break;
    case 12: akw = AU_K_progressive; break;
    case 13: akw = d->hover ? AU_K_hover : AU_K_none; break;
    case 14: akw = d->any_hover ? AU_K_hover : AU_K_none; break;
    case 15: akw = d->pointer == 2 ? AU_K_fine : d->pointer == 1 ? AU_K_coarse : AU_K_none; break;
    case 16: akw = d->any_pointer == 2 ? AU_K_fine : d->any_pointer == 1 ? AU_K_coarse : AU_K_none; break;
    case 17: akw = d->dark ? AU_K_dark : AU_K_light; break;
    case 18: akw = d->reduced_motion ? AU_K_reduce : AU_K_no_preference; break;
    case 19: akw = d->reduced_transparency ? AU_K_reduce : AU_K_no_preference; break;
    case 20: akw = d->contrast_more ? AU_K_more : AU_K_no_preference; break;
    case 21: akw = d->forced_colors ? AU_K_active : AU_K_none; break;
    case 22: akw = AU_K_none; break;
    case 23: akw = AU_K_browser; break;
    case 24: akw = d->scripting == 2 ? AU_K_enabled : d->scripting == 1 ? AU_K_initial_only : AU_K_none; break;
    case 25: akw = d->print ? AU_K_none : AU_K_fast; break;
    case 26: case 27: akw = AU_K_standard; break;
    case 28: akw = AU_K_srgb; break;
    case 29: akw = d->print ? AU_K_none : AU_K_scroll; break;
    case 30: akw = AU_K_scroll; break;
    case 32: akw = AU_K_no_preference; break;
    default: return false;
    }
    if (n->op[0] == OP_NONE) {
        /* boolean context */
        if (type == FT_DISCRETE)
            return f == 10 ? d->grid != 0 : (akw != AU_K_none && akw != AU_K_no_preference);
        return actual != 0;
    }
    for (int k = 0; k < 2; k++) {
        if (n->op[k] == OP_NONE)
            continue;
        if (type == FT_DISCRETE) {
            if (f == 10) {
                if (n->vt[k] != VT_NUMBER || !cmp(actual, OP_EQ, n->v[k]))
                    return false;
                continue;
            }
            if (n->vt[k] != VT_IDENT || n->kw[k] != akw)
                return false;
            /* color-gamut: srgb is the only gamut matched */
            continue;
        }
        float want;
        switch (type) {
        case FT_RANGE_LEN:
            if (n->vt[k] == VT_NUMBER && n->v[k] == 0)
                want = 0;
            else if (n->vt[k] == VT_LENGTH)
                want = len_px(n->v[k], n->unit[k], d);
            else
                return false;
            break;
        case FT_RANGE_RATIO:
            if (n->vt[k] != VT_RATIO && n->vt[k] != VT_NUMBER)
                return false;
            want = n->v[k];
            break;
        case FT_RANGE_RES:
            if (f == 31 && n->vt[k] == VT_NUMBER)
                want = n->v[k];
            else if (n->vt[k] == VT_RES)
                want = n->v[k];
            else
                return false;
            break;
        default: /* integer */
            if (n->vt[k] != VT_NUMBER)
                return false;
            want = n->v[k];
            break;
        }
        if (!cmp(actual, n->op[k], want))
            return false;
    }
    return true;
}

static bool eval_node(const AuCssMediaList *m, int32_t i, const AuCssDevice *d, int depth)
{
    if (i < 0 || depth > 64)
        return false;
    const MqNode *n = &m->n[i];
    switch (n->kind) {
    case MQ_QUERY_LIST: {
        if (n->child < 0)
            return true; /* empty list: all */
        for (int32_t c = n->child; c >= 0; c = m->n[c].next)
            if (eval_node(m, c, d, depth + 1))
                return true;
        return false;
    }
    case MQ_AND:
        for (int32_t c = n->child; c >= 0; c = m->n[c].next)
            if (!eval_node(m, c, d, depth + 1))
                return false;
        return true;
    case MQ_OR:
        for (int32_t c = n->child; c >= 0; c = m->n[c].next)
            if (eval_node(m, c, d, depth + 1))
                return true;
        return false;
    case MQ_NOT:
        return !eval_node(m, n->child, d, depth + 1);
    case MQ_TYPE:
        switch (n->kw[0]) {
        case AU_K_all: return true;
        case AU_K_screen: return !d->print;
        case AU_K_print: return d->print != 0;
        default: return false;
        }
    case MQ_CONST:
        return n->value;
    case MQ_FEATURE:
        return eval_feature(n, d);
    default:
        return false;
    }
}

bool css_media_eval(const AuCssMediaList *m, const AuCssDevice *d)
{
    if (!m)
        return true;
    return eval_node(m, 0, d, 0);
}

void au_css_device_default(AuCssDevice *d)
{
    memset(d, 0, sizeof *d);
    d->width = 1024;
    d->height = 768;
    d->device_width = 1024;
    d->device_height = 768;
    d->resolution = 1;
    d->color_bits = 8;
    d->hover = d->any_hover = 1;
    d->pointer = d->any_pointer = 2;
    d->scripting = 2;
}

/* ---- @supports ---- */

static bool sup_condition(const CssTokens *tk, uint32_t *i, uint32_t b, int depth);

static bool sup_decl(const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t i = sk(tk, a, b);
    if (i >= b || tk->t[i].type != TK_IDENT)
        return false;
    const CssToken *name = &tk->t[i];
    uint32_t k = sk(tk, i + 1, b);
    if (k >= b || tk->t[k].type != TK_COLON)
        return false;
    k++;
    const char *nm = css_tok_str(tk, name);
    if (css_ident_is_custom_prop(nm, name->len))
        return true;
    uint16_t prop = css_prop_lookup(nm, name->len);
    if (!prop)
        return false;
    uint32_t e = b;
    while (e > k && tk->t[e - 1].type == TK_WS)
        e--;
    /* !important is allowed */
    if (e > k && tk->t[e - 1].type == TK_IDENT && au_ieq(css_tok_str(tk, &tk->t[e - 1]), tk->t[e - 1].len,
                                                           "important", 9)) {
        uint32_t q = e - 1;
        while (q > k && tk->t[q - 1].type == TK_WS)
            q--;
        if (q > k && tk->t[q - 1].type == TK_DELIM && tk->t[q - 1].num == '!')
            e = q - 1;
    }
    AuCssDeclBlock tmp;
    css_decl_block_init(&tmp);
    bool ok = css_parse_property(prop, tk, k, e, false, &tmp, false, NULL);
    css_decl_block_clear(&tmp);
    return ok;
}

static bool sup_in_parens(const CssTokens *tk, uint32_t *i, uint32_t b, int depth, bool *valid)
{
    uint32_t k = sk(tk, *i, b);
    *valid = true;
    if (k >= b || depth > 32) {
        *valid = false;
        return false;
    }
    const CssToken *t = &tk->t[k];
    uint32_t end = t->len;
    if (t->type == TK_FUNCTION) {
        uint32_t s = k + 1, e = (end > s && tk->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
        *i = end;
        if (t->kw == AU_K_selector) {
            CssSelParseCtx sc = {0};
            AuSelList *l = css_selector_parse(tk, s, e, &sc);
            if (!l)
                return false;
            css_sellist_free(l);
            return true;
        }
        if (au_ieq(css_tok_str(tk, t), css_tok_len(t), "font-format", 11)) {
            /* the formats @font-face src accepts for the font loader */
            static const char *const formats[] = {"woff2", "woff", "truetype", "opentype", "collection"};
            uint32_t q = sk(tk, s, e);
            if (q >= e || tk->t[q].type != TK_IDENT || sk(tk, q + 1, e) != e)
                return false;
            for (size_t f = 0; f < sizeof formats / sizeof formats[0]; f++)
                if (au_ieq(css_tok_str(tk, &tk->t[q]), tk->t[q].len, formats[f], strlen(formats[f])))
                    return true;
            return false;
        }
        return false; /* font-tech() (no color or variation font technology) and general-enclosed */
    }
    if (t->type != TK_LPAREN) {
        *valid = false;
        return false;
    }
    uint32_t s = k + 1, e = (end > s && tk->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
    *i = end;
    uint32_t q = sk(tk, s, e);
    if (q < e && (tk->t[q].type == TK_LPAREN || ident_is(tk, q, "not") || tk->t[q].type == TK_FUNCTION)) {
        uint32_t qq = q;
        bool r = sup_condition(tk, &qq, e, depth + 1);
        if (sk(tk, qq, e) == e)
            return r;
    }
    return sup_decl(tk, s, e);
}

static bool sup_condition(const CssTokens *tk, uint32_t *i, uint32_t b, int depth)
{
    uint32_t k = sk(tk, *i, b);
    bool valid;
    if (k < b && ident_is(tk, k, "not")) {
        k++;
        bool r = sup_in_parens(tk, &k, b, depth, &valid);
        *i = k;
        return valid && !r;
    }
    bool r = sup_in_parens(tk, &k, b, depth, &valid);
    if (!valid) {
        *i = b + 1; /* force a mismatch */
        return false;
    }
    int kind = 0;
    for (;;) {
        uint32_t q = sk(tk, k, b);
        if (q >= b)
            break;
        int this_kind = ident_is(tk, q, "and") ? 1 : ident_is(tk, q, "or") ? 2 : 0;
        if (!this_kind || (kind && this_kind != kind)) {
            *i = b + 1;
            return false;
        }
        kind = this_kind;
        q++;
        bool c = sup_in_parens(tk, &q, b, depth, &valid);
        if (!valid) {
            *i = b + 1;
            return false;
        }
        r = kind == 1 ? (r && c) : (r || c);
        k = q;
    }
    *i = k;
    return r;
}

bool css_supports_eval(const CssTokens *tk, uint32_t a, uint32_t b)
{
    uint32_t i = a;
    bool r = sup_condition(tk, &i, b, 0);
    return r && sk(tk, i, b) == b;
}

bool au_css_supports(const char *cond, size_t len)
{
    au_css_init();
    CssTokens tk = {0};
    css_tokenize(cond, len, &tk);
    bool r = css_supports_eval(&tk, 0, tk.n);
    if (!r) {
        /* CSS.supports("display: flex") without parentheses */
        AuBuf b;
        au_buf_init(&b);
        au_buf_putc(&b, '(');
        au_buf_append(&b, cond, len);
        au_buf_putc(&b, ')');
        CssTokens t2 = {0};
        css_tokenize(b.data, b.len, &t2);
        uint32_t k = sk(&t2, 1, t2.n);
        if (k < t2.n && t2.t[k].type == TK_IDENT)
            r = css_supports_eval(&t2, 0, t2.n);
        css_tokens_free(&t2);
        au_buf_free(&b);
    }
    css_tokens_free(&tk);
    return r;
}

bool au_css_supports_decl(const char *prop, size_t plen, const char *value, size_t vlen)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_putc(&b, '(');
    au_buf_append(&b, prop, plen);
    au_buf_putc(&b, ':');
    au_buf_append(&b, value, vlen);
    au_buf_putc(&b, ')');
    bool r = au_css_supports(b.data, b.len);
    au_buf_free(&b);
    return r;
}
