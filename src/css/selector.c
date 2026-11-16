/* Selector parsing (Selectors Level 4) and specificity.
 *
 * A complex selector compiles into an array of components in right-to-left
 * order: the subject's compound first, then a SC_COMBINATOR component, the
 * next compound to the left, and so on. Nested lists (:is, :not, :has,
 * :nth-child(... of S)) hang off their component.
 *
 * Nesting: in a nested style rule, & refers to the parent rule's
 * selectors. When the parent is a single selector without a
 * pseudo-element and & occurs only in the leftmost compound, its
 * components are copied in place (exactly equivalent, and indexable);
 * otherwise & stays a SC_NESTING component that matches like :is(parent). */
#include "css_int.h"

#define MAX_SEL_DEPTH 16
/* The most compounds a complex selector may have, and the most components
 * once a parent selector is inlined for "&". Matching recurses once per
 * combinator, and the component count is kept in 16 bits; a longer
 * selector is invalid. */
#define MAX_SEL_COMPOUNDS 128
#define MAX_SEL_COMPONENTS 4096

/* A component while its selector is being parsed. The selector's final
 * components (AuSelComp, in the arena) are written once it is known to be
 * valid; an attribute value or :lang string is on the heap until then. */
typedef struct {
    uint8_t type, op, flags, ns;
    AuAtom name, name_cs;
    union {
        struct {
            char *s;
            uint32_t len;
        } value;
        AuSelList *list;
        struct {
            int32_t a, b;
            AuSelList *of;
        } nth;
    } u;
} TComp;

typedef struct {
    TComp *c;
    uint32_t n, cap;
    uint8_t comb;   /* combinator to the compound on the left (set on the right compound) */
    bool has_amp;
} Compound;

typedef struct {
    const CssTokens *tk;
    const CssSelParseCtx *cx;
    bool failed;
    AuAtom pe_name; /* name of an unknown or functional pseudo-element, as written */
} SP;

static AuSelList *parse_list(const CssTokens *tk, uint32_t a, uint32_t b, const CssSelParseCtx *cx);

static bool tcomp_has_string(const TComp *c)
{
    return c->type == SC_ATTR || (c->type == SC_PSEUDO && c->op == PC_LANG);
}

static void tcomp_free(TComp *c)
{
    if (tcomp_has_string(c))
        au_free(c->u.value.s);
}

uint32_t css_sellist_max_spec(const AuSelList *l)
{
    uint32_t m = 0;
    if (!l)
        return 0;
    for (uint32_t i = 0; i < l->n; i++)
        if (l->sel[i].spec > m)
            m = l->sel[i].spec;
    return m;
}

static uint32_t spec_add(uint32_t a, uint32_t b)
{
    uint32_t x = ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF);
    uint32_t y = ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF);
    uint32_t z = (a & 0xFF) + (b & 0xFF);
    if (x > 255) x = 255;
    if (y > 255) y = 255;
    if (z > 255) z = 255;
    return (x << 16) | (y << 8) | z;
}

static uint32_t sk(const CssTokens *tk, uint32_t i, uint32_t b)
{
    while (i < b && tk->t[i].type == TK_WS)
        i++;
    return i;
}

static AuAtom lower_atom(const char *s, size_t n)
{
    char buf[256];
    if (n < sizeof buf) {
        css_lower(s, n, buf, sizeof buf);
        return au_atom(buf, n);
    }
    char *p = au_alloc(n + 1);
    css_lower(s, n, p, n + 1);
    AuAtom a = au_atom(p, n);
    au_free(p);
    return a;
}

static TComp *push_comp(Compound *c, int type)
{
    TComp *x = CSS_PUSH(c->c, c->n, c->cap);
    memset(x, 0, sizeof *x);
    x->type = (uint8_t)type;
    return x;
}

static bool is_delim(const CssToken *t, int c) { return t->type == TK_DELIM && t->num == c; }

/* ---- An+B ---- */

static bool parse_anb(const CssTokens *tk, uint32_t a, uint32_t b, int32_t *A, int32_t *B)
{
    AuBuf buf;
    au_buf_init(&buf);
    for (uint32_t i = a; i < b; i++) {
        const CssToken *t = &tk->t[i];
        if (t->type == TK_WS)
            continue;
        if (t->type == TK_NUMBER && !(t->flags & TF_INTEGER)) {
            au_buf_free(&buf);
            return false;
        }
        if (t->type == TK_DIMENSION && !(t->flags & TF_INTEGER)) {
            au_buf_free(&buf);
            return false;
        }
        if (t->type != TK_IDENT && t->type != TK_NUMBER && t->type != TK_DIMENSION && t->type != TK_DELIM) {
            au_buf_free(&buf);
            return false;
        }
        css_tokens_serialize(tk, i, i + 1, &buf);
    }
    char *s = au_buf_cstr(&buf);
    for (char *q = s; *q; q++)
        *q = (char)au_lower((unsigned char)*q);
    bool ok = false;
    if (strcmp(s, "odd") == 0) {
        *A = 2;
        *B = 1;
        ok = true;
    } else if (strcmp(s, "even") == 0) {
        *A = 2;
        *B = 0;
        ok = true;
    } else {
        const char *p = s;
        int sign = 1;
        if (*p == '+' || *p == '-') {
            sign = *p == '-' ? -1 : 1;
            p++;
        }
        long num = 0;
        bool digits = false;
        while (au_is_digit((unsigned char)*p)) {
            num = num * 10 + (*p - '0');
            if (num > 100000000)
                num = 100000000;
            digits = true;
            p++;
        }
        if (*p == 'n') {
            *A = (int32_t)(sign * (digits ? num : 1));
            p++;
            if (!*p) {
                *B = 0;
                ok = true;
            } else if (*p == '+' || *p == '-') {
                int s2 = *p == '-' ? -1 : 1;
                p++;
                long bn = 0;
                bool d2 = false;
                while (au_is_digit((unsigned char)*p)) {
                    bn = bn * 10 + (*p - '0');
                    if (bn > 100000000)
                        bn = 100000000;
                    d2 = true;
                    p++;
                }
                if (d2 && !*p) {
                    *B = (int32_t)(s2 * bn);
                    ok = true;
                }
            }
        } else if (digits && !*p) {
            *A = 0;
            *B = (int32_t)(sign * num);
            ok = true;
        }
    }
    au_buf_free(&buf);
    return ok;
}

/* ---- Pseudo-classes and pseudo-elements ---- */

static const struct {
    const char *name;
    uint8_t pc;
} pseudo_classes[] = {
    {"first-child", PC_FIRST_CHILD}, {"last-child", PC_LAST_CHILD}, {"only-child", PC_ONLY_CHILD},
    {"first-of-type", PC_FIRST_OF_TYPE}, {"last-of-type", PC_LAST_OF_TYPE}, {"only-of-type", PC_ONLY_OF_TYPE},
    {"root", PC_ROOT}, {"empty", PC_EMPTY}, {"link", PC_LINK}, {"any-link", PC_ANY_LINK},
    {"-webkit-any-link", PC_ANY_LINK}, {"visited", PC_VISITED}, {"local-link", PC_LOCAL_LINK},
    {"hover", PC_HOVER}, {"active", PC_ACTIVE}, {"focus", PC_FOCUS}, {"focus-visible", PC_FOCUS_VISIBLE},
    {"focus-within", PC_FOCUS_WITHIN}, {"checked", PC_CHECKED}, {"indeterminate", PC_INDETERMINATE},
    {"default", PC_DEFAULT}, {"disabled", PC_DISABLED}, {"enabled", PC_ENABLED}, {"required", PC_REQUIRED},
    {"optional", PC_OPTIONAL}, {"read-only", PC_READ_ONLY}, {"read-write", PC_READ_WRITE},
    {"placeholder-shown", PC_PLACEHOLDER_SHOWN}, {"target", PC_TARGET}, {"defined", PC_DEFINED},
    {"scope", PC_SCOPE}, {"valid", PC_VALID}, {"invalid", PC_INVALID}, {"user-valid", PC_USER_VALID},
    {"user-invalid", PC_USER_INVALID}, {"in-range", PC_IN_RANGE}, {"out-of-range", PC_OUT_OF_RANGE},
    {"autofill", PC_NEVER}, {"-webkit-autofill", PC_NEVER}, {"fullscreen", PC_NEVER},
    {"-webkit-full-screen", PC_NEVER}, {"modal", PC_MODAL}, {"popover-open", PC_POPOVER_OPEN},
    {"picture-in-picture", PC_NEVER}, {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"seeking", PC_NEVER},
    {"buffering", PC_NEVER}, {"stalled", PC_NEVER}, {"muted", PC_NEVER}, {"volume-locked", PC_NEVER},
    {"open", PC_NEVER}, {"closed", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER},
    {"future", PC_NEVER}, {"blank", PC_NEVER}, {"target-within", PC_NEVER}, {"host", PC_NEVER},
    {"-moz-focusring", PC_NEVER}, {"-moz-ui-invalid", PC_NEVER}, {"-webkit-autofill-strong-password", PC_NEVER},
    {"placeholder", PC_NEVER}, {"first", PC_NEVER}, {"left", PC_NEVER}, {"right", PC_NEVER},
};

static int pseudo_class_lookup(const char *s, size_t n)
{
    char buf[48];
    if (!css_lower(s, n, buf, sizeof buf))
        return 0;
    for (size_t i = 0; i < CSS_ARRAY_LEN(pseudo_classes); i++)
        if (strcmp(pseudo_classes[i].name, buf) == 0)
            return pseudo_classes[i].pc;
    return 0;
}

static int pseudo_element_lookup(const char *s, size_t n, bool legacy_only)
{
    char buf[64];
    if (!css_lower(s, n, buf, sizeof buf))
        return -1;
    if (strcmp(buf, "before") == 0) return AU_CSS_PSEUDO_BEFORE;
    if (strcmp(buf, "after") == 0) return AU_CSS_PSEUDO_AFTER;
    if (strcmp(buf, "first-line") == 0) return AU_CSS_PSEUDO_FIRST_LINE;
    if (strcmp(buf, "first-letter") == 0) return AU_CSS_PSEUDO_FIRST_LETTER;
    if (legacy_only)
        return -1;
    if (strcmp(buf, "marker") == 0) return AU_CSS_PSEUDO_MARKER;
    if (strcmp(buf, "placeholder") == 0 || strcmp(buf, "-webkit-input-placeholder") == 0)
        return AU_CSS_PSEUDO_PLACEHOLDER;
    if (strcmp(buf, "selection") == 0) return AU_CSS_PSEUDO_SELECTION;
    if (strcmp(buf, "backdrop") == 0) return AU_CSS_PSEUDO_BACKDROP;
    if (strcmp(buf, "file-selector-button") == 0 || strcmp(buf, "-webkit-file-upload-button") == 0)
        return AU_CSS_PSEUDO_FILE_SELECTOR_BUTTON;
    if (strncmp(buf, "-webkit-", 8) == 0)
        return AU_CSS_PSEUDO_UNKNOWN_WEBKIT;
    return -1;
}

/* ---- Compound parsing ---- */

/* Parses a namespace prefix and name: [ns|]name, *|name, |name.
 * On success sets *name (or 0 for *), flags and ns. */
static bool parse_qname(SP *p, uint32_t *i, uint32_t b, const CssToken **name_tok, bool *universal, uint8_t *flags,
                        uint8_t *ns, bool attr)
{
    const CssTokens *tk = p->tk;
    uint32_t k = *i;
    *flags = 0;
    *ns = 0;
    *universal = false;
    *name_tok = NULL;
    const CssToken *t = &tk->t[k];
    /* prefix forms */
    if (k + 1 < b && is_delim(&tk->t[k + 1], '|') && (t->type == TK_IDENT || is_delim(t, '*')) &&
        k + 2 < b && (tk->t[k + 2].type == TK_IDENT || (!attr && is_delim(&tk->t[k + 2], '*')))) {
        if (is_delim(t, '*')) {
            *flags |= SCF_NS_ANY;
        } else {
            AuAtom pre = au_atom(css_tok_str(tk, t), t->len);
            int nsv = -2;
            bool found = false;
            if (p->cx->ns)
                for (uint32_t q = 0; q < p->cx->ns->n; q++)
                    if (p->cx->ns->prefixes[q] == pre) {
                        nsv = p->cx->ns->nss[q];
                        found = true;
                    }
            if (!found)
                return false; /* undeclared prefix: invalid selector */
            *flags |= SCF_NS_SET;
            *ns = (uint8_t)(nsv < 0 ? 255 : nsv);
        }
        k += 2;
        t = &tk->t[k];
    } else if (is_delim(t, '|') && k + 1 < b && (tk->t[k + 1].type == TK_IDENT || (!attr && is_delim(&tk->t[k + 1], '*')))) {
        *flags |= SCF_NS_NONE;
        k++;
        t = &tk->t[k];
    } else if (!attr && p->cx->ns && p->cx->ns->default_ns != -1) {
        *flags |= SCF_NS_SET;
        *ns = (uint8_t)(p->cx->ns->default_ns < 0 ? 255 : p->cx->ns->default_ns);
    }
    if (t->type == TK_IDENT) {
        *name_tok = t;
    } else if (!attr && is_delim(t, '*')) {
        *universal = true;
    } else {
        return false;
    }
    *i = k + 1;
    return true;
}

static bool parse_attr(SP *p, uint32_t a, uint32_t b, Compound *c)
{
    const CssTokens *tk = p->tk;
    uint32_t i = sk(tk, a, b);
    if (i >= b)
        return false;
    const CssToken *nt;
    bool uni;
    uint8_t flags, ns;
    if (!parse_qname(p, &i, b, &nt, &uni, &flags, &ns, true) || !nt)
        return false;
    TComp *x = push_comp(c, SC_ATTR);
    x->name = lower_atom(css_tok_str(tk, nt), nt->len);
    x->name_cs = au_atom(css_tok_str(tk, nt), nt->len);
    x->flags = flags;
    x->ns = ns;
    i = sk(tk, i, b);
    if (i >= b) {
        x->op = ATTR_EXISTS;
        return true;
    }
    const CssToken *t = &tk->t[i];
    int op = 0;
    if (is_delim(t, '=')) {
        op = ATTR_EQ;
        i++;
    } else if (t->type == TK_DELIM && i + 1 < b && is_delim(&tk->t[i + 1], '=')) {
        switch ((int)t->num) {
        case '~': op = ATTR_INCLUDES; break;
        case '|': op = ATTR_DASH; break;
        case '^': op = ATTR_PREFIX; break;
        case '$': op = ATTR_SUFFIX; break;
        case '*': op = ATTR_SUBSTR; break;
        default: return false;
        }
        i += 2;
    } else {
        return false;
    }
    x = &c->c[c->n - 1];
    x->op = (uint8_t)op;
    i = sk(tk, i, b);
    if (i >= b || (tk->t[i].type != TK_IDENT && tk->t[i].type != TK_STRING))
        return false;
    x->u.value.s = au_strndup(css_tok_str(tk, &tk->t[i]), tk->t[i].len);
    x->u.value.len = tk->t[i].len;
    i = sk(tk, i + 1, b);
    if (i < b) {
        if (tk->t[i].type != TK_IDENT || tk->t[i].len != 1)
            return false;
        int f = au_lower((unsigned char)css_tok_str(tk, &tk->t[i])[0]);
        if (f == 'i')
            x->flags |= SCF_CI;
        else if (f == 's')
            x->flags |= SCF_CS;
        else
            return false;
        if (sk(tk, i + 1, b) != b)
            return false;
    }
    return true;
}

static bool parse_pseudo_fn(SP *p, const CssToken *f, uint32_t s, uint32_t e, Compound *c, uint32_t *spec)
{
    const CssTokens *tk = p->tk;
    char name[32];
    if (!css_lower(css_tok_str(tk, f), css_tok_len(f), name, sizeof name))
        return false;
    CssSelParseCtx sub = *p->cx;
    sub.depth++;
    sub.nested_rule = false;
    sub.relative = false;
    sub.forgiving = false;
    if (sub.depth > MAX_SEL_DEPTH)
        return false;
    int pc = 0;
    if (!strcmp(name, "is") || !strcmp(name, "matches") || !strcmp(name, "-webkit-any") || !strcmp(name, "-moz-any"))
        pc = PC_IS;
    else if (!strcmp(name, "where"))
        pc = PC_WHERE;
    else if (!strcmp(name, "not"))
        pc = PC_NOT;
    else if (!strcmp(name, "has"))
        pc = PC_HAS;
    if (pc) {
        sub.forgiving = pc == PC_IS || pc == PC_WHERE;
        sub.relative = pc == PC_HAS;
        if (pc == PC_HAS && p->cx->relative && p->cx->depth > 0)
            return false; /* :has() inside :has() */
        AuSelList *l = parse_list(tk, s, e, &sub);
        if (!l)
            return false;
        /* pseudo-elements are not allowed in these lists; :is() and
         * :where() drop them, :not() and :has() fail */
        for (uint32_t i = 0; i < l->n; i++) {
            if (!l->sel[i].pseudo)
                continue;
            if (pc == PC_HAS || pc == PC_NOT)
                return false;
            memmove(&l->sel[i], &l->sel[i + 1], (l->n - i - 1) * sizeof(AuSelector));
            l->n--;
            i--;
        }
        TComp *x = push_comp(c, SC_PSEUDO);
        x->op = (uint8_t)pc;
        x->u.list = l;
        *spec = pc == PC_WHERE ? 0 : css_sellist_max_spec(l);
        return true;
    }
    if (!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child") || !strcmp(name, "nth-of-type") ||
        !strcmp(name, "nth-last-of-type")) {
        pc = !strcmp(name, "nth-child") ? PC_NTH_CHILD : !strcmp(name, "nth-last-child") ? PC_NTH_LAST_CHILD
             : !strcmp(name, "nth-of-type") ? PC_NTH_OF_TYPE : PC_NTH_LAST_OF_TYPE;
        uint32_t of = e;
        for (uint32_t i = s; i < e; i++)
            if (tk->t[i].type == TK_IDENT && tk->t[i].kw == AU_K_of) {
                of = i;
                break;
            }
        int32_t A, B;
        if (!parse_anb(tk, s, of, &A, &B))
            return false;
        AuSelList *ofl = NULL;
        if (of < e) {
            if (pc != PC_NTH_CHILD && pc != PC_NTH_LAST_CHILD)
                return false;
            ofl = parse_list(tk, of + 1, e, &sub);
            if (!ofl)
                return false;
        }
        TComp *x = push_comp(c, SC_PSEUDO);
        x->op = (uint8_t)pc;
        x->u.nth.a = A;
        x->u.nth.b = B;
        x->u.nth.of = ofl;
        *spec = spec_add(1 << 8, ofl ? css_sellist_max_spec(ofl) : 0);
        return true;
    }
    if (!strcmp(name, "lang")) {
        AuBuf buf;
        au_buf_init(&buf);
        bool want = true;
        for (uint32_t i = s; i < e; i++) {
            const CssToken *t = &tk->t[i];
            if (t->type == TK_WS)
                continue;
            if (t->type == TK_COMMA && !want) {
                want = true;
                continue;
            }
            if (!want || (t->type != TK_IDENT && t->type != TK_STRING)) {
                au_buf_free(&buf);
                return false;
            }
            if (buf.len)
                au_buf_putc(&buf, ',');
            au_buf_append(&buf, css_tok_str(tk, t), t->len);
            want = false;
        }
        if (want) {
            au_buf_free(&buf);
            return false;
        }
        TComp *x = push_comp(c, SC_PSEUDO);
        x->op = PC_LANG;
        x->u.value.len = (uint32_t)buf.len;
        x->u.value.s = au_buf_take(&buf, NULL);
        *spec = 1 << 8;
        return true;
    }
    if (!strcmp(name, "dir")) {
        uint32_t i = sk(tk, s, e);
        if (i >= e || tk->t[i].type != TK_IDENT || sk(tk, i + 1, e) != e)
            return false;
        uint16_t kw = tk->t[i].kw;
        if (kw != AU_K_ltr && kw != AU_K_rtl)
            return false;
        TComp *x = push_comp(c, SC_PSEUDO);
        x->op = PC_DIR;
        x->name = kw == AU_K_ltr ? 0 : 1;
        *spec = 1 << 8;
        return true;
    }
    if (!strcmp(name, "host") || !strcmp(name, "host-context") || !strcmp(name, "state") ||
        !strcmp(name, "current") || !strcmp(name, "past") || !strcmp(name, "future")) {
        TComp *x = push_comp(c, SC_PSEUDO);
        x->op = PC_NEVER;
        *spec = 1 << 8;
        return true;
    }
    return false;
}

/* Parses one compound at *i; stops at whitespace, a combinator or b. */
static bool parse_compound(SP *p, uint32_t *i, uint32_t b, Compound *c, uint32_t *spec, uint8_t *pseudo)
{
    const CssTokens *tk = p->tk;
    uint32_t k = *i;
    bool any = false;
    bool after_pseudo_el = false;
    /* type or universal */
    if (k < b && (tk->t[k].type == TK_IDENT || is_delim(&tk->t[k], '*') || is_delim(&tk->t[k], '|'))) {
        const CssToken *nt;
        bool uni;
        uint8_t flags, ns;
        if (!parse_qname(p, &k, b, &nt, &uni, &flags, &ns, false))
            return false;
        TComp *x = push_comp(c, uni ? SC_UNIVERSAL : SC_TAG);
        x->flags = flags;
        x->ns = ns;
        if (!uni) {
            x->name = lower_atom(css_tok_str(tk, nt), nt->len);
            x->name_cs = au_atom(css_tok_str(tk, nt), nt->len);
            *spec = spec_add(*spec, 1);
        }
        any = true;
    }
    while (k < b) {
        const CssToken *t = &tk->t[k];
        if (after_pseudo_el && t->type != TK_COLON)
            return false;
        if (t->type == TK_HASH) {
            if (!(t->flags & TF_ID))
                return false;
            TComp *x = push_comp(c, SC_ID);
            x->name = au_atom(css_tok_str(tk, t), t->len);
            *spec = spec_add(*spec, 1 << 16);
            k++;
        } else if (is_delim(t, '.')) {
            if (k + 1 >= b || tk->t[k + 1].type != TK_IDENT)
                return false;
            TComp *x = push_comp(c, SC_CLASS);
            x->name = au_atom(css_tok_str(tk, &tk->t[k + 1]), tk->t[k + 1].len);
            *spec = spec_add(*spec, 1 << 8);
            k += 2;
        } else if (t->type == TK_LBRACKET) {
            uint32_t end = t->len;
            uint32_t e = (end > k + 1 && tk->t[end - 1].type == TK_RBRACKET) ? end - 1 : end;
            if (!parse_attr(p, k + 1, e, c))
                return false;
            *spec = spec_add(*spec, 1 << 8);
            k = end;
        } else if (is_delim(t, '&')) {
            TComp *x = push_comp(c, SC_NESTING);
            x->u.list = (AuSelList *)p->cx->parent;
            c->has_amp = true;
            k++;
        } else if (t->type == TK_COLON) {
            bool el = k + 1 < b && tk->t[k + 1].type == TK_COLON;
            uint32_t q = k + (el ? 2 : 1);
            if (q >= b)
                return false;
            const CssToken *nt = &tk->t[q];
            if (el || (nt->type == TK_IDENT && pseudo_element_lookup(css_tok_str(tk, nt), nt->len, true) >= 0)) {
                if (*pseudo && !el)
                    return false;
                if (nt->type != TK_IDENT) {
                    /* functional pseudo-elements (::part, ::slotted, ::highlight) never match */
                    if (nt->type != TK_FUNCTION)
                        return false;
                    *pseudo = AU_CSS_PSEUDO_UNKNOWN_WEBKIT;
                    AuBuf nb;
                    au_buf_init(&nb);
                    css_tokens_serialize(tk, q, nt->len, &nb);
                    p->pe_name = au_atom(nb.data ? nb.data : "", nb.len);
                    au_buf_free(&nb);
                    k = nt->len;
                } else {
                    int pe = pseudo_element_lookup(css_tok_str(tk, nt), nt->len, false);
                    if (pe < 0)
                        return false;
                    if (*pseudo)
                        pe = AU_CSS_PSEUDO_UNKNOWN_WEBKIT; /* nested pseudo-elements */
                    if (pe == AU_CSS_PSEUDO_UNKNOWN_WEBKIT)
                        p->pe_name = au_atom(css_tok_str(tk, nt), nt->len);
                    *pseudo = (uint8_t)pe;
                    k = q + 1;
                }
                *spec = spec_add(*spec, 1);
                after_pseudo_el = true;
                any = true;
                continue;
            }
            if (nt->type == TK_IDENT) {
                int pc = pseudo_class_lookup(css_tok_str(tk, nt), nt->len);
                if (!pc)
                    return false;
                if (after_pseudo_el || *pseudo) {
                    /* user-action pseudo-classes on a pseudo-element: never matches */
                    if (pc != PC_HOVER && pc != PC_ACTIVE && pc != PC_FOCUS && pc != PC_FOCUS_VISIBLE &&
                        pc != PC_PLACEHOLDER_SHOWN)
                        return false;
                    *pseudo = AU_CSS_PSEUDO_UNKNOWN_WEBKIT;
                }
                TComp *x = push_comp(c, SC_PSEUDO);
                x->op = (uint8_t)pc;
                *spec = spec_add(*spec, 1 << 8);
                k = q + 1;
            } else if (nt->type == TK_FUNCTION) {
                if (after_pseudo_el || *pseudo)
                    return false;
                uint32_t end = nt->len;
                uint32_t e = (end > q + 1 && tk->t[end - 1].type == TK_RPAREN) ? end - 1 : end;
                uint32_t sp = 0;
                if (!parse_pseudo_fn(p, nt, q + 1, e, c, &sp))
                    return false;
                *spec = spec_add(*spec, sp);
                k = end;
            } else {
                return false;
            }
        } else {
            break;
        }
        any = true;
    }
    *i = k;
    return any;
}

/* ---- Complex selectors ---- */

static uint32_t compound_spec_nesting(const Compound *c)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < c->n; i++)
        if (c->c[i].type == SC_NESTING)
            s = spec_add(s, css_sellist_max_spec(c->c[i].u.list));
    return s;
}

/* Hashes for the ancestor filter (match.c computes element hashes the
 * same way): fed the components in their final order, right to left. */
typedef struct {
    uint32_t h[CSS_SEL_HASHES];
    uint32_t n;
    bool ancestor;
} HashFeed;

static void hash_feed(HashFeed *f, int type, int op, AuAtom name)
{
    if (type == SC_COMBINATOR) {
        f->ancestor = op == COMB_DESCENDANT || op == COMB_CHILD;
        return;
    }
    if (!f->ancestor || f->n >= CSS_SEL_HASHES)
        return;
    uint32_t h = 0;
    if (type == SC_ID)
        h = css_hash_atom(name, 1);
    else if (type == SC_CLASS)
        h = css_hash_atom(name, 2);
    else if (type == SC_TAG)
        h = css_hash_atom(name, 3);
    if (h)
        f->h[f->n++] = h;
}

static void free_compounds(Compound *cs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t k = 0; k < cs[i].n; k++)
            tcomp_free(&cs[i].c[k]);
        au_free(cs[i].c);
    }
    au_free(cs);
}

/* Writes the final form of a parsed component into the arena and takes
 * its string. */
static void emit_comp(CssArena *A, AuSelComp *dst, TComp *src)
{
    dst->type = src->type;
    dst->op = src->op;
    dst->flags = src->flags;
    dst->ns = src->ns;
    dst->u.ext = NULL;
    bool is_list = src->type == SC_NESTING ||
                   (src->type == SC_PSEUDO &&
                    (src->op == PC_IS || src->op == PC_WHERE || src->op == PC_NOT || src->op == PC_HAS));
    bool is_nth = src->type == SC_PSEUDO && src->op >= PC_NTH_CHILD && src->op <= PC_NTH_LAST_OF_TYPE;
    bool has_string = tcomp_has_string(src) && (src->type != SC_ATTR || src->op != ATTR_EXISTS);
    if (is_list) {
        dst->u.list = src->u.list;
        return;
    }
    if (!is_nth && !has_string && src->name == src->name_cs) {
        dst->u.name = src->name;
        tcomp_free(src);
        return;
    }
    if (!is_nth && !has_string && src->type != SC_TAG && src->type != SC_ATTR) {
        dst->u.name = src->name; /* name_cs is not set for these */
        return;
    }
    CssSelExt *x = css_arena_alloc_ptr(A, sizeof *x);
    memset(x, 0, sizeof *x);
    x->name = src->name;
    x->name_cs = src->name_cs;
    if (is_nth) {
        x->u.nth.a = src->u.nth.a;
        x->u.nth.b = src->u.nth.b;
        x->u.nth.of = src->u.nth.of;
    } else if (has_string) {
        char *v = css_arena_alloc_ptr(A, (size_t)src->u.value.len + 1);
        if (src->u.value.len)
            memcpy(v, src->u.value.s, src->u.value.len);
        v[src->u.value.len] = 0;
        x->u.value.s = v;
        x->u.value.len = src->u.value.len;
    }
    tcomp_free(src);
    dst->flags |= SCF_EXT;
    dst->u.ext = x;
}

/* Parses one complex (or relative) selector in [a, b). */
static bool parse_complex(SP *p, uint32_t a, uint32_t b, AuSelector *out)
{
    const CssTokens *tk = p->tk;
    const CssSelParseCtx *cx = p->cx;
    Compound *cs = NULL;
    uint32_t nc = 0, ccap = 0;
    uint32_t spec = 0;
    uint8_t pseudo = 0;
    uint8_t lead = 0;
    p->pe_name = 0;
    while (b > a && tk->t[b - 1].type == TK_WS)
        b--; /* trailing whitespace is not a combinator */
    uint32_t i = sk(tk, a, b);
    if (i >= b)
        return false;
    /* a leading combinator (relative selectors) */
    if (tk->t[i].type == TK_DELIM && (tk->t[i].num == '>' || tk->t[i].num == '+' || tk->t[i].num == '~')) {
        if (!cx->relative)
            return false;
        lead = tk->t[i].num == '>' ? COMB_CHILD : tk->t[i].num == '+' ? COMB_NEXT : COMB_SUBSEQUENT;
        i = sk(tk, i + 1, b);
    }
    uint8_t pending = 0;
    for (;;) {
        if (i >= b)
            break;
        if (pseudo) {
            /* nothing may follow a pseudo-element compound */
            free_compounds(cs, nc);
            return false;
        }
        if (nc >= MAX_SEL_COMPOUNDS) {
            free_compounds(cs, nc);
            return false;
        }
        Compound *c = CSS_PUSH(cs, nc, ccap);
        memset(c, 0, sizeof *c);
        c->comb = pending;
        if (!parse_compound(p, &i, b, c, &spec, &pseudo)) {
            free_compounds(cs, nc);
            return false;
        }
        /* combinator */
        uint32_t q = i;
        bool ws = q < b && tk->t[q].type == TK_WS;
        q = sk(tk, q, b);
        if (q >= b)
            break;
        const CssToken *t = &tk->t[q];
        if (t->type == TK_DELIM && (t->num == '>' || t->num == '+' || t->num == '~')) {
            pending = t->num == '>' ? COMB_CHILD : t->num == '+' ? COMB_NEXT : COMB_SUBSEQUENT;
            i = sk(tk, q + 1, b);
            if (i >= b) {
                free_compounds(cs, nc);
                return false;
            }
        } else if (ws) {
            pending = COMB_DESCENDANT;
            i = q;
        } else {
            free_compounds(cs, nc);
            return false;
        }
    }
    if (!nc) {
        free_compounds(cs, nc);
        return false;
    }
    /* nesting: implicit & and & inlining */
    bool amp = false;
    for (uint32_t k = 0; k < nc; k++)
        amp = amp || cs[k].has_amp;
    if (cx->nested_rule && !amp) {
        css_grow((void **)&cs, &ccap, nc + 1, sizeof(Compound));
        memmove(cs + 1, cs, nc * sizeof(Compound));
        nc++;
        memset(&cs[0], 0, sizeof cs[0]);
        TComp *x = push_comp(&cs[0], SC_NESTING);
        x->u.list = (AuSelList *)cx->parent;
        cs[0].has_amp = true;
        cs[1].comb = lead ? lead : COMB_DESCENDANT;
        lead = 0;
        amp = true;
    }
    uint32_t amp_spec = 0;
    for (uint32_t k = 0; k < nc; k++)
        amp_spec = spec_add(amp_spec, compound_spec_nesting(&cs[k]));
    bool inline_amp = false;
    const AuSelector *ps = NULL;
    if (amp && cx->parent && cx->parent->n == 1 && !cx->parent->sel[0].pseudo && !cx->parent->sel[0].rel_comb) {
        inline_amp = true;
        for (uint32_t k = 1; k < nc; k++)
            if (cs[k].has_amp)
                inline_amp = false;
        ps = &cx->parent->sel[0];
    }
    /* emit right to left */
    uint32_t total = 0;
    for (uint32_t k = 0; k < nc; k++)
        total += cs[k].n + 1;
    if (inline_amp)
        total += ps->ncomp;
    if (total > MAX_SEL_COMPONENTS) {
        free_compounds(cs, nc);
        return false;
    }
    /* the filter hashes, from the components in their final order */
    HashFeed hf;
    memset(&hf, 0, sizeof hf);
    uint32_t count = 0; /* the components written below: at most total */
    for (uint32_t k = nc; k-- > 0;) {
        Compound *c = &cs[k];
        for (uint32_t m = 0; m < c->n; m++)
            if (!(inline_amp && c->c[m].type == SC_NESTING)) {
                hash_feed(&hf, c->c[m].type, c->c[m].op, c->c[m].name);
                count++;
            }
        if (inline_amp && c->has_amp)
            for (uint32_t m = 0; m < ps->ncomp; m++) {
                hash_feed(&hf, ps->comp[m].type, ps->comp[m].op, css_comp_name(&ps->comp[m]));
                count++;
            }
        if (k > 0) {
            hash_feed(&hf, SC_COMBINATOR, c->comb, 0);
            count++;
        }
    }
    bool named = pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT;
    AuSelComp *out_c =
        css_arena_alloc_ptr(cx->arena, count * sizeof(AuSelComp) + (hf.n + (named ? 1 : 0)) * sizeof(uint32_t));
    uint32_t w = 0;
    uint8_t flags = 0;
    for (uint32_t k = nc; k-- > 0;) {
        Compound *c = &cs[k];
        for (uint32_t m = 0; m < c->n; m++) {
            if (inline_amp && c->c[m].type == SC_NESTING)
                continue;
            emit_comp(cx->arena, &out_c[w++], &c->c[m]);
        }
        if (inline_amp && c->has_amp) {
            /* the parent's rightmost compound joins this one, its other
             * compounds follow to the left; what hangs off them is in the
             * same arena and is shared */
            for (uint32_t m = 0; m < ps->ncomp; m++)
                out_c[w++] = ps->comp[m];
        }
        if (k > 0) {
            AuSelComp *x = &out_c[w++];
            memset(x, 0, sizeof *x);
            x->type = SC_COMBINATOR;
            x->op = c->comb;
        }
    }
    for (uint32_t k = 0; k < nc; k++)
        au_free(cs[k].c);
    au_free(cs);
    memset(out, 0, sizeof *out);
    out->comp = out_c;
    out->ncomp = (uint16_t)w;
    out->pseudo = pseudo;
    out->rel_comb = cx->relative && !cx->nested_rule ? (lead ? lead : COMB_DESCENDANT) : 0;
    if (inline_amp)
        out->spec = spec_add(spec, ps->spec) & 0xFFFFFF;
    else
        out->spec = spec_add(spec, amp_spec) & 0xFFFFFF;
    out->nhash = (uint8_t)hf.n;
    uint32_t *tail = (uint32_t *)(out_c + w);
    for (uint32_t k = 0; k < hf.n; k++)
        tail[k] = hf.h[k];
    if (named)
        tail[hf.n] = p->pe_name;
    for (uint32_t k = 0; k < w; k++) {
        const AuSelComp *c = &out_c[k];
        if (c->type == SC_PSEUDO) {
            if (c->op == PC_NEVER)
                flags |= SELF_NEVER;
            if (c->op == PC_HAS)
                flags |= SELF_HAS;
            if (c->op == PC_HOVER || c->op == PC_ACTIVE || c->op == PC_FOCUS || c->op == PC_FOCUS_VISIBLE ||
                c->op == PC_FOCUS_WITHIN || c->op == PC_CHECKED || c->op == PC_TARGET)
                flags |= SELF_STATE;
            if ((c->op == PC_IS || c->op == PC_WHERE || c->op == PC_NOT || c->op == PC_HAS) && c->u.list)
                flags |= c->u.list->flags;
            if (c->op >= PC_NTH_CHILD && c->op <= PC_NTH_LAST_OF_TYPE && c->u.ext->u.nth.of)
                flags |= c->u.ext->u.nth.of->flags;
        } else if (c->type == SC_NESTING) {
            flags |= SELF_NESTED;
            if (c->u.list)
                flags |= c->u.list->flags;
        } else if (c->type == SC_ATTR) {
            flags |= SELF_ATTR;
        }
    }
    if (pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT)
        flags |= SELF_NEVER;
    out->flags = flags;
    return true;
}

static AuSelList *parse_list(const CssTokens *tk, uint32_t a, uint32_t b, const CssSelParseCtx *cx)
{
    SP p = {tk, cx, false, 0};
    AuSelector *sel = NULL; /* collected here, then copied into the arena */
    uint32_t n = 0, cap = 0;
    uint8_t flags = 0;
    uint32_t start = a;
    for (uint32_t i = a; i <= b;) {
        if (i == b || tk->t[i].type == TK_COMMA) {
            AuSelector s;
            if (n < CSS_SELLIST_MAX && parse_complex(&p, start, i, &s)) {
                *CSS_PUSH(sel, n, cap) = s;
                flags |= s.flags & (uint8_t)~SELF_NEVER;
            } else if (!cx->forgiving) {
                au_free(sel);
                return NULL;
            }
            if (i == b)
                break;
            i++;
            start = i;
            continue;
        }
        int ty = tk->t[i].type;
        i = (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET || ty == TK_LBRACE) ? tk->t[i].len : i + 1;
    }
    if (!n && !cx->forgiving) {
        au_free(sel);
        return NULL;
    }
    AuSelList *l = css_arena_alloc_ptr(cx->arena, sizeof *l + n * sizeof(AuSelector));
    l->n = n;
    l->flags = flags;
    if (n)
        memcpy(l->sel, sel, n * sizeof(AuSelector));
    au_free(sel);
    return l;
}

AuSelList *css_selector_parse(const CssTokens *tk, uint32_t a, uint32_t b, const CssSelParseCtx *cx)
{
    return parse_list(tk, a, b, cx);
}

/* ---- Serialization ---- */

static void ser_list(const AuSelList *l, AuBuf *out);

static void ser_comp(const AuSelComp *c, AuBuf *out)
{
    switch (c->type) {
    case SC_TAG:
        if (c->flags & SCF_NS_ANY)
            au_buf_puts(out, "*|");
        else if (c->flags & SCF_NS_NONE)
            au_buf_putc(out, '|');
        css_serialize_ident(au_atom_str(css_comp_name_cs(c)), au_atom_len(css_comp_name_cs(c)), out);
        break;
    case SC_UNIVERSAL:
        if (c->flags & SCF_NS_ANY)
            au_buf_puts(out, "*|");
        else if (c->flags & SCF_NS_NONE)
            au_buf_putc(out, '|');
        au_buf_putc(out, '*');
        break;
    case SC_ID:
        au_buf_putc(out, '#');
        css_serialize_ident(au_atom_str(c->u.name), au_atom_len(c->u.name), out);
        break;
    case SC_CLASS:
        au_buf_putc(out, '.');
        css_serialize_ident(au_atom_str(c->u.name), au_atom_len(c->u.name), out);
        break;
    case SC_ATTR: {
        au_buf_putc(out, '[');
        css_serialize_ident(au_atom_str(css_comp_name_cs(c)), au_atom_len(css_comp_name_cs(c)), out);
        static const char *ops[] = {"", "", "=", "~=", "|=", "^=", "$=", "*="};
        if (c->op != ATTR_EXISTS) {
            uint32_t vlen;
            const char *v = css_comp_value(c, &vlen);
            au_buf_puts(out, ops[c->op]);
            css_serialize_string(v ? v : "", vlen, out);
            if (c->flags & SCF_CI)
                au_buf_puts(out, " i");
            else if (c->flags & SCF_CS)
                au_buf_puts(out, " s");
        }
        au_buf_putc(out, ']');
        break;
    }
    case SC_NESTING:
        au_buf_putc(out, '&');
        break;
    case SC_PSEUDO: {
        const char *nm = NULL;
        for (size_t i = 0; i < CSS_ARRAY_LEN(pseudo_classes); i++)
            if (pseudo_classes[i].pc == c->op) {
                nm = pseudo_classes[i].name;
                break;
            }
        switch (c->op) {
        case PC_IS: case PC_WHERE: case PC_NOT: case PC_HAS:
            au_buf_puts(out, c->op == PC_IS ? ":is(" : c->op == PC_WHERE ? ":where(" : c->op == PC_NOT ? ":not(" : ":has(");
            ser_list(c->u.list, out);
            au_buf_putc(out, ')');
            return;
        case PC_NTH_CHILD: case PC_NTH_LAST_CHILD: case PC_NTH_OF_TYPE: case PC_NTH_LAST_OF_TYPE: {
            static const char *n[] = {":nth-child(", ":nth-last-child(", ":nth-of-type(", ":nth-last-of-type("};
            au_buf_puts(out, n[c->op - PC_NTH_CHILD]);
            int32_t A = c->u.ext->u.nth.a, B = c->u.ext->u.nth.b;
            if (A == 0)
                au_buf_printf(out, "%ld", (long)B);
            else {
                if (A == 1)
                    au_buf_puts(out, "n");
                else if (A == -1)
                    au_buf_puts(out, "-n");
                else
                    au_buf_printf(out, "%ldn", (long)A);
                if (B > 0)
                    au_buf_printf(out, "+%ld", (long)B);
                else if (B < 0)
                    au_buf_printf(out, "%ld", (long)B);
            }
            if (c->u.ext->u.nth.of) {
                au_buf_puts(out, " of ");
                ser_list(c->u.ext->u.nth.of, out);
            }
            au_buf_putc(out, ')');
            return;
        }
        case PC_LANG: {
            uint32_t vlen;
            const char *v = css_comp_value(c, &vlen);
            au_buf_puts(out, ":lang(");
            au_buf_append(out, v, vlen);
            au_buf_putc(out, ')');
            return;
        }
        case PC_DIR:
            au_buf_puts(out, c->u.name ? ":dir(rtl)" : ":dir(ltr)");
            return;
        default:
            au_buf_putc(out, ':');
            au_buf_puts(out, nm ? nm : "unknown");
            return;
        }
    }
    default:
        break;
    }
}

static const char *pseudo_el_name(int p)
{
    switch (p) {
    case AU_CSS_PSEUDO_BEFORE: return "::before";
    case AU_CSS_PSEUDO_AFTER: return "::after";
    case AU_CSS_PSEUDO_MARKER: return "::marker";
    case AU_CSS_PSEUDO_PLACEHOLDER: return "::placeholder";
    case AU_CSS_PSEUDO_SELECTION: return "::selection";
    case AU_CSS_PSEUDO_FIRST_LINE: return "::first-line";
    case AU_CSS_PSEUDO_FIRST_LETTER: return "::first-letter";
    case AU_CSS_PSEUDO_BACKDROP: return "::backdrop";
    case AU_CSS_PSEUDO_FILE_SELECTOR_BUTTON: return "::file-selector-button";
    case AU_CSS_PSEUDO_UNKNOWN_WEBKIT: return "::-webkit-unknown";
    default: return "";
    }
}

static void ser_selector(const AuSelector *s, AuBuf *out)
{
    /* components are right to left; walk compounds from the left */
    uint32_t ends[256], ncomp = 0;
    for (uint32_t i = 0; i <= s->ncomp && ncomp < 256; i++)
        if (i == s->ncomp || s->comp[i].type == SC_COMBINATOR)
            ends[ncomp++] = i;
    if (s->rel_comb && s->rel_comb != COMB_DESCENDANT)
        au_buf_puts(out, s->rel_comb == COMB_CHILD ? "> " : s->rel_comb == COMB_NEXT ? "+ " : "~ ");
    for (uint32_t k = ncomp; k-- > 0;) {
        uint32_t b = ends[k], a = k ? ends[k - 1] + 1 : 0;
        for (uint32_t i = a; i < b; i++)
            ser_comp(&s->comp[i], out);
        if (k == 0 && s->pseudo) {
            if (css_sel_pseudo_name(s)) {
                au_buf_puts(out, "::");
                au_buf_puts(out, au_atom_str(css_sel_pseudo_name(s)));
            } else {
                au_buf_puts(out, pseudo_el_name(s->pseudo));
            }
        }
        if (k > 0) {
            int comb = s->comp[ends[k - 1]].op;
            au_buf_puts(out, comb == COMB_DESCENDANT ? " " : comb == COMB_CHILD ? " > " : comb == COMB_NEXT ? " + " : " ~ ");
        }
    }
}

static void ser_list(const AuSelList *l, AuBuf *out)
{
    for (uint32_t i = 0; l && i < l->n; i++) {
        if (i)
            au_buf_puts(out, ", ");
        ser_selector(&l->sel[i], out);
    }
}

void css_sellist_serialize(const AuSelList *l, AuBuf *out) { ser_list(l, out); }

/* ---- Public selector API ---- */

struct AuCssSelectorList {
    CssArena arena; /* holds l */
    AuSelList *l;
};

AuCssSelectorList *au_css_selector_parse(const char *text, size_t len)
{
    au_css_init();
    CssTokens tk = {0};
    css_tokenize(text, len, &tk);
    AuCssSelectorList *r = au_calloc(sizeof *r);
    r->arena.chunk = len < 64 ? 176 : 496; /* most are a few components */
    r->arena.chunk_max = 8176;
    CssSelParseCtx cx = {0};
    cx.arena = &r->arena;
    r->l = css_selector_parse(&tk, 0, tk.n, &cx);
    css_tokens_free(&tk);
    if (!r->l) {
        au_css_selector_free(r);
        return NULL;
    }
    return r;
}

void au_css_selector_free(AuCssSelectorList *l)
{
    if (!l)
        return;
    css_arena_free(&l->arena);
    au_free(l);
}

bool au_css_selector_matches_scoped(const AuCssSelectorList *l, AuElement *el, const AuNode *scope)
{
    CssMatchCtx mc = {0};
    mc.quirks = el->node.doc && el->node.doc->quirks == AU_QUIRKS_FULL;
    mc.scope = scope && scope->type == AU_NODE_ELEMENT ? (const AuElement *)scope : NULL;
    for (uint32_t i = 0; i < l->l->n; i++)
        if (!l->l->sel[i].pseudo && css_selector_match(&l->l->sel[i], el, &mc))
            return true;
    return false;
}

bool au_css_selector_matches(const AuCssSelectorList *l, AuElement *el)
{
    return au_css_selector_matches_scoped(l, el, &el->node);
}

AuElement *au_css_query_selector(const AuCssSelectorList *l, AuNode *root)
{
    for (AuNode *n = root->first_child ? root->first_child : NULL; n; n = au_node_next(n, root))
        if (n->type == AU_NODE_ELEMENT && au_css_selector_matches_scoped(l, (AuElement *)n, root))
            return (AuElement *)n;
    return NULL;
}
