/* Selector matching and the ancestor filter.
 *
 * Matching goes right to left. Recursion follows combinators of one
 * selector (a descendant combinator tries each ancestor in a loop and
 * recurses into the rest of the selector) and nested selector lists, so
 * its depth is bounded by the selector, not by the document.
 *
 * :has() is evaluated directly by walking the anchor's subtree (or its
 * following siblings) with au_node_next; style invalidation treats any
 * sheet with :has() coarsely (cascade.c). */
#include "css_int.h"

#include "../dom/control.h"

/* ---- Atom caches ---- */

static uint32_t *ci_hash;
static AuAtom *lower_of;
static uint32_t ci_n, lower_n;

static void grow_u32(uint32_t **arr, uint32_t *n, uint32_t need)
{
    if (need < *n)
        return;
    uint32_t nn = *n ? *n : 1024;
    while (nn <= need)
        nn *= 2;
    *arr = au_realloc(*arr, nn * sizeof(uint32_t));
    memset(*arr + *n, 0, (nn - *n) * sizeof(uint32_t));
    *n = nn;
}

static uint32_t ci_string_hash(AuAtom a)
{
    const char *s = au_atom_str(a);
    size_t n = au_atom_len(a);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint32_t)au_lower((unsigned char)s[i])) * 16777619u;
    return h ? h : 1;
}

uint32_t css_hash_atom(AuAtom a, int salt)
{
    grow_u32(&ci_hash, &ci_n, a);
    uint32_t h = ci_hash[a];
    if (!h)
        h = ci_hash[a] = ci_string_hash(a);
    h += (uint32_t)salt * 0x9E3779B9u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h ? h : 1;
}

AuAtom css_atom_lower(AuAtom a)
{
    grow_u32((uint32_t **)&lower_of, &lower_n, a);
    if (lower_of[a])
        return lower_of[a];
    const char *s = au_atom_str(a);
    size_t n = au_atom_len(a);
    bool has_upper = false;
    for (size_t i = 0; i < n; i++)
        if (s[i] >= 'A' && s[i] <= 'Z')
            has_upper = true;
    AuAtom r = a;
    if (has_upper) {
        char *buf = au_alloc(n + 1);
        css_lower(s, n, buf, n + 1);
        r = au_atom(buf, n);
        au_free(buf);
        grow_u32((uint32_t **)&lower_of, &lower_n, a); /* au_atom may have added atoms; a is unchanged */
    }
    lower_of[a] = r;
    return r;
}

/* ---- Ancestor filter ---- */

void css_bloom_clear(CssBloom *b) { memset(b->c, 0, sizeof b->c); }

static void bloom_add(CssBloom *b, uint32_t h, int d)
{
    uint32_t m = (1u << CSS_BLOOM_BITS) - 1;
    uint32_t i1 = h & m, i2 = (h >> CSS_BLOOM_BITS) & m;
    if (d > 0) {
        if (b->c[i1] < 255)
            b->c[i1]++;
        if (b->c[i2] < 255)
            b->c[i2]++;
    } else {
        if (b->c[i1] && b->c[i1] < 255)
            b->c[i1]--;
        if (b->c[i2] && b->c[i2] < 255)
            b->c[i2]--;
    }
}

static void bloom_element(CssBloom *b, const AuElement *el, int d)
{
    if (el->id)
        bloom_add(b, css_hash_atom(el->id, 1), d);
    for (uint32_t i = 0; i < el->nclasses; i++)
        bloom_add(b, css_hash_atom(el->classes[i], 2), d);
    bloom_add(b, css_hash_atom(el->tag, 3), d);
}

void css_bloom_push(CssBloom *b, const AuElement *el, bool quirks)
{
    (void)quirks; /* hashes are case-insensitive either way */
    bloom_element(b, el, 1);
}

void css_bloom_pop(CssBloom *b, const AuElement *el, bool quirks)
{
    (void)quirks;
    bloom_element(b, el, -1);
}

/* ---- Tree helpers ---- */

static const AuElement *parent_el(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return (p && p->type == AU_NODE_ELEMENT) ? (const AuElement *)p : NULL;
}

static const AuElement *prev_el(const AuElement *el)
{
    for (AuNode *n = el->node.prev; n; n = n->prev)
        if (n->type == AU_NODE_ELEMENT)
            return (const AuElement *)n;
    return NULL;
}

static const AuElement *next_el(const AuElement *el)
{
    for (AuNode *n = el->node.next; n; n = n->next)
        if (n->type == AU_NODE_ELEMENT)
            return (const AuElement *)n;
    return NULL;
}

static bool is_root(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_DOCUMENT;
}

static bool is_html(const AuElement *el) { return el->ns == AU_NS_HTML; }

static bool is_html_tag(const AuElement *el, AuAtom tag) { return el->ns == AU_NS_HTML && el->tag == tag; }

static const char *attr_val(const AuElement *el, AuAtom name) { return au_element_get_attr(el, name); }

/* ---- Attributes ---- */

static const char *const ci_attr_names[] = {
    "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked", "clear",
    "codetype", "color", "compact", "declare", "defer", "dir", "direction", "disabled", "enctype", "face",
    "frame", "hreflang", "http-equiv", "lang", "language", "link", "media", "method", "multiple", "nohref",
    "noresize", "noshade", "nowrap", "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected",
    "shape", "target", "text", "type", "valign", "valuetype", "vlink",
};
static AuAtom ci_attr_atoms[CSS_ARRAY_LEN(ci_attr_names)];

static bool attr_value_ci(AuAtom name)
{
    if (!ci_attr_atoms[0])
        for (size_t i = 0; i < CSS_ARRAY_LEN(ci_attr_names); i++)
            ci_attr_atoms[i] = au_atom_cstr(ci_attr_names[i]);
    for (size_t i = 0; i < CSS_ARRAY_LEN(ci_attr_atoms); i++)
        if (ci_attr_atoms[i] == name)
            return true;
    return false;
}

static bool str_eq(const char *a, size_t an, const char *b, size_t bn, bool ci)
{
    if (an != bn)
        return false;
    return ci ? au_ieq(a, an, b, bn) : memcmp(a, b, an) == 0;
}

static bool match_attr_value(const AuSelComp *c, const char *v, size_t vn, bool ci)
{
    uint32_t wlen;
    const char *w = css_comp_value(c, &wlen);
    size_t wn = wlen;
    switch (c->op) {
    case ATTR_EXISTS:
        return true;
    case ATTR_EQ:
        return str_eq(v, vn, w, wn, ci);
    case ATTR_INCLUDES: {
        if (!wn)
            return false;
        for (size_t i = 0; i < wn; i++)
            if (au_is_space((unsigned char)w[i]))
                return false;
        size_t i = 0;
        while (i < vn) {
            while (i < vn && au_is_space((unsigned char)v[i]))
                i++;
            size_t s = i;
            while (i < vn && !au_is_space((unsigned char)v[i]))
                i++;
            if (i > s && str_eq(v + s, i - s, w, wn, ci))
                return true;
        }
        return false;
    }
    case ATTR_DASH:
        if (vn == wn)
            return str_eq(v, vn, w, wn, ci);
        return vn > wn && v[wn] == '-' && str_eq(v, wn, w, wn, ci);
    case ATTR_PREFIX:
        return wn && vn >= wn && str_eq(v, wn, w, wn, ci);
    case ATTR_SUFFIX:
        return wn && vn >= wn && str_eq(v + vn - wn, wn, w, wn, ci);
    case ATTR_SUBSTR:
        if (!wn || vn < wn)
            return false;
        for (size_t i = 0; i + wn <= vn; i++)
            if (str_eq(v + i, wn, w, wn, ci))
                return true;
        return false;
    default:
        return false;
    }
}

static bool match_attr(const AuSelComp *c, const AuElement *el)
{
    bool html = is_html(el);
    AuAtom want = html ? css_comp_name(c) : css_comp_name_cs(c);
    for (uint32_t i = 0; i < el->nattrs; i++) {
        const AuAttr *a = &el->attrs[i];
        bool name_ok;
        if (c->flags & (SCF_NS_ANY | SCF_NS_SET)) {
            /* match the local part of a qualified name */
            const char *s = au_atom_str(a->name);
            const char *colon = strchr(s, ':');
            const char *local = colon ? colon + 1 : s;
            const char *w = au_atom_str(want);
            name_ok = strcmp(local, w) == 0;
            if (name_ok && (c->flags & SCF_NS_SET)) {
                int ns = c->ns;
                name_ok = (ns == 255) ? false
                          : (a->ns == AU_ATTR_NS_NONE ? false : true); /* only xlink/xml attrs carry namespaces */
                (void)ns;
            }
        } else {
            name_ok = a->name == want && a->ns == AU_ATTR_NS_NONE;
            if (!name_ok && html && a->ns == AU_ATTR_NS_NONE && a->name != want)
                name_ok = false;
        }
        if (!name_ok)
            continue;
        bool ci;
        if (c->flags & SCF_CI)
            ci = true;
        else if (c->flags & SCF_CS)
            ci = false;
        else
            ci = html && attr_value_ci(css_comp_name(c));
        if (match_attr_value(c, a->value, a->len, ci))
            return true;
    }
    return false;
}

/* ---- Pseudo-class helpers ---- */

static bool nth_ok(int32_t a, int32_t b, int32_t index)
{
    if (a == 0)
        return index == b;
    int32_t d = index - b;
    if ((a > 0 && d < 0) || (a < 0 && d > 0))
        return false;
    return d % a == 0;
}

static bool same_type(const AuElement *a, const AuElement *b) { return a->tag == b->tag && a->ns == b->ns; }

static bool is_form_control(const AuElement *el)
{
    if (!is_html(el))
        return false;
    AuAtom t = el->tag;
    return t == HN(button) || t == HN(input) || t == HN(select) || t == HN(textarea) || t == HN(optgroup) ||
           t == HN(option) || t == HN(fieldset);
}

static bool actually_disabled(const AuElement *el)
{
    if (!is_form_control(el))
        return false;
    if (attr_val(el, HN(disabled)))
        return true;
    AuAtom t = el->tag;
    if (t == HN(option)) {
        const AuElement *p = parent_el(el);
        return p && is_html_tag(p, HN(optgroup)) && attr_val(p, HN(disabled));
    }
    if (t == HN(optgroup))
        return false;
    /* a descendant of a disabled fieldset, outside its first legend */
    const AuElement *child = el;
    for (const AuElement *p = parent_el(el); p; child = p, p = parent_el(p)) {
        if (is_html_tag(p, HN(fieldset)) && attr_val(p, HN(disabled))) {
            const AuElement *legend = NULL;
            for (AuNode *n = p->node.first_child; n; n = n->next)
                if (n->type == AU_NODE_ELEMENT && is_html_tag((AuElement *)n, HN(legend))) {
                    legend = (AuElement *)n;
                    break;
                }
            if (child != legend)
                return true;
        }
    }
    return false;
}

static bool type_is(const AuElement *el, const char *t)
{
    const char *v = attr_val(el, HN(type));
    if (!v)
        return strcmp(t, "text") == 0;
    return au_ieq(v, strlen(v), t, strlen(t));
}

static bool is_checkable(const AuElement *el)
{
    return is_html_tag(el, HN(input)) && (type_is(el, "checkbox") || type_is(el, "radio"));
}

static bool is_checked(const AuElement *el)
{
    if (is_checkable(el)) {
        if (el->state & AU_STATE_CHECKED_SET)
            return (el->state & AU_STATE_CHECKED) != 0;
        return attr_val(el, HN(checked)) != NULL;
    }
    if (is_html_tag(el, HN(option))) {
        if (el->state & AU_STATE_CHECKED_SET)
            return (el->state & AU_STATE_CHECKED) != 0;
        return attr_val(el, HN(selected)) != NULL;
    }
    return false;
}

static bool is_text_input(const AuElement *el)
{
    static const char *const mutable_types[] = {"text", "search", "url", "tel", "email", "password", "date",
                                                "month", "week", "time", "datetime-local", "number"};
    if (!is_html_tag(el, HN(input)))
        return false;
    const char *v = attr_val(el, HN(type));
    if (!v)
        return true;
    for (size_t i = 0; i < CSS_ARRAY_LEN(mutable_types); i++)
        if (au_ieq(v, strlen(v), mutable_types[i], strlen(mutable_types[i])))
            return true;
    /* unknown types are text */
    static const char *const known[] = {"hidden", "checkbox", "radio", "file", "submit", "image", "reset",
                                        "button", "range", "color"};
    for (size_t i = 0; i < CSS_ARRAY_LEN(known); i++)
        if (au_ieq(v, strlen(v), known[i], strlen(known[i])))
            return false;
    return true;
}

static bool is_editable(const AuElement *el)
{
    for (const AuElement *e = el; e; e = parent_el(e)) {
        const char *v = attr_val(e, HN(contenteditable));
        if (v) {
            if (!*v || au_ieq(v, strlen(v), "true", 4) || au_ieq(v, strlen(v), "plaintext-only", 14))
                return true;
            if (au_ieq(v, strlen(v), "false", 5))
                return false;
        }
    }
    return false;
}

static bool read_write(const AuElement *el)
{
    if (is_text_input(el) || is_html_tag(el, HN(textarea)))
        return !attr_val(el, HN(readonly)) && !actually_disabled(el);
    return is_editable(el);
}

static bool has_text_content(const AuElement *el)
{
    for (AuNode *n = el->node.first_child; n; n = n->next)
        if (n->type == AU_NODE_TEXT && ((AuText *)n)->len)
            return true;
    return false;
}

/* The control's live value once the user or script has changed it (the
 * dirty value flag); until then the value is the default, which the
 * callers read from the value attribute or the textarea's text. */
static const AuControl *live_control(const AuElement *el)
{
    const AuControl *c = au_ctl_peek(el);
    return c && c->dirty ? c : NULL;
}

static bool value_empty(const AuElement *el)
{
    const AuControl *c = live_control(el);
    if (c)
        return c->value.len == 0;
    if (is_html_tag(el, HN(textarea)))
        return !has_text_content(el);
    const char *v = attr_val(el, HN(value));
    return !v || !*v;
}

static bool lang_matches(const AuElement *el, const char *ranges, size_t rlen)
{
    const char *lang = NULL;
    for (const AuElement *e = el; e && !lang; e = parent_el(e)) {
        lang = attr_val(e, HN(xml_lang));
        if (!lang)
            lang = attr_val(e, HN(lang));
    }
    if (!lang)
        return false;
    size_t ln = strlen(lang);
    size_t i = 0;
    while (i < rlen) {
        size_t s = i;
        while (i < rlen && ranges[i] != ',')
            i++;
        const char *r = ranges + s;
        size_t rn = i - s;
        i++;
        if (rn == 1 && r[0] == '*') {
            if (ln)
                return true;
            continue;
        }
        if (rn >= 2 && r[0] == '*' && r[1] == '-') {
            /* "*-CH": any language with that subtag */
            for (size_t k = 0; k + rn - 1 <= ln; k++)
                if (lang[k] == '-' && au_ieq(lang + k, rn - 1 > ln - k ? ln - k : rn - 1, r + 1, rn - 1) &&
                    (k + rn - 1 == ln || lang[k + rn - 1] == '-'))
                    return true;
            continue;
        }
        if (ln >= rn && au_ieq(lang, rn, r, rn) && (ln == rn || lang[rn] == '-'))
            return true;
    }
    return false;
}

static int directionality(const AuElement *el)
{
    for (const AuElement *e = el; e; e = parent_el(e)) {
        const char *d = attr_val(e, HN(dir));
        if (d) {
            if (au_ieq(d, strlen(d), "rtl", 3))
                return 1;
            if (au_ieq(d, strlen(d), "ltr", 3))
                return 0;
            if (au_ieq(d, strlen(d), "auto", 4))
                return 0; /* text-based detection is not done */
        }
    }
    return 0;
}

static bool is_link(const AuElement *el)
{
    return (is_html_tag(el, HN(a)) || is_html_tag(el, HN(area))) && attr_val(el, HN(href));
}

static bool constraint_invalid(const AuElement *el)
{
    if (!is_html(el))
        return false;
    if ((is_text_input(el) || is_html_tag(el, HN(textarea)) || is_checkable(el)) && attr_val(el, HN(required)) &&
        !actually_disabled(el)) {
        if (is_checkable(el))
            return !is_checked(el);
        return value_empty(el);
    }
    return false;
}

static bool validation_candidate(const AuElement *el)
{
    return is_html(el) && (is_html_tag(el, HN(input)) || is_html_tag(el, HN(textarea)) ||
                           is_html_tag(el, HN(select)) || is_html_tag(el, HN(form)) ||
                           is_html_tag(el, HN(fieldset)));
}

static bool range_state(const AuElement *el, bool *in_range)
{
    if (!is_html_tag(el, HN(input)) || !(type_is(el, "number") || type_is(el, "range")))
        return false;
    const char *mn = attr_val(el, HN(min)), *mx = attr_val(el, HN(max)), *v = attr_val(el, HN(value));
    if (!mn && !mx)
        return false;
    char live[48];
    const AuControl *c = live_control(el);
    if (c) {
        size_t n = c->value.len < sizeof live - 1 ? c->value.len : sizeof live - 1;
        if (n)
            memcpy(live, c->value.data, n);
        live[n] = 0;
        v = live;
    }
    *in_range = true;
    if (v && *v) {
        double x = 0, lo = 0, hi = 0;
        /* a simple decimal parse: these attributes are plain numbers */
        x = 0;
        int sign = 1;
        const char *p = v;
        if (*p == '-') {
            sign = -1;
            p++;
        }
        double frac = 0, scale = 1;
        bool dot = false;
        for (; *p; p++) {
            if (*p == '.') {
                dot = true;
                continue;
            }
            if (!au_is_digit((unsigned char)*p))
                break;
            if (dot) {
                scale /= 10;
                frac += (*p - '0') * scale;
            } else {
                x = x * 10 + (*p - '0');
            }
        }
        x = sign * (x + frac);
        if (mn) {
            lo = 0;
            sign = 1;
            p = mn;
            if (*p == '-') {
                sign = -1;
                p++;
            }
            while (au_is_digit((unsigned char)*p))
                lo = lo * 10 + (*p++ - '0');
            lo *= sign;
            if (x < lo)
                *in_range = false;
        }
        if (mx) {
            hi = 0;
            sign = 1;
            p = mx;
            if (*p == '-') {
                sign = -1;
                p++;
            }
            while (au_is_digit((unsigned char)*p))
                hi = hi * 10 + (*p++ - '0');
            hi *= sign;
            if (x > hi)
                *in_range = false;
        }
    }
    return true;
}

/* ---- Matching ---- */

static bool match_from(const AuSelector *s, uint32_t i, const AuElement *el, const CssMatchCtx *mc,
                       const AuElement *anchor);

static bool list_match(const AuSelList *l, const AuElement *el, const CssMatchCtx *mc)
{
    if (!l)
        return false;
    for (uint32_t i = 0; i < l->n; i++)
        if (!l->sel[i].pseudo && match_from(&l->sel[i], 0, el, mc, NULL))
            return true;
    return false;
}

/* ---- The :has() descendant summary ---- */

enum { HB_CHECKED = 60, HB_FOCUS = 61, HB_HOVER = 62 };

static uint64_t hbit(uint32_t h) { return (uint64_t)1 << (h % 60); }

uint64_t css_has_element_bits(const AuElement *el)
{
    uint64_t m = hbit(css_hash_atom(el->tag, 3));
    if (el->id)
        m |= hbit(css_hash_atom(el->id, 1));
    for (uint32_t i = 0; i < el->nclasses; i++)
        m |= hbit(css_hash_atom(el->classes[i], 2));
    for (uint32_t i = 0; i < el->nattrs; i++) {
        AuAtom n = el->attrs[i].name;
        m |= hbit(css_hash_atom(n, 4));
        if (n == HN(checked) || n == HN(selected))
            m |= (uint64_t)1 << HB_CHECKED;
    }
    if (el->state & AU_STATE_CHECKED)
        m |= (uint64_t)1 << HB_CHECKED;
    if (el->state & (AU_STATE_FOCUS | AU_STATE_FOCUS_VISIBLE | AU_STATE_FOCUS_WITHIN))
        m |= (uint64_t)1 << HB_FOCUS;
    if (el->state & (AU_STATE_HOVER | AU_STATE_ACTIVE))
        m |= (uint64_t)1 << HB_HOVER;
    return m;
}

/* The bits every compound of a relative selector needs: each of them
 * matches a descendant of the anchor. */
uint64_t css_has_selector_bits(const AuSelector *s)
{
    uint64_t m = 0;
    for (uint32_t i = 0; i < s->ncomp; i++) {
        const AuSelComp *c = &s->comp[i];
        switch (c->type) {
        case SC_TAG: m |= hbit(css_hash_atom(css_comp_name(c), 3)); break;
        case SC_ID: m |= hbit(css_hash_atom(c->u.name, 1)); break;
        case SC_CLASS: m |= hbit(css_hash_atom(c->u.name, 2)); break;
        case SC_ATTR: m |= hbit(css_hash_atom(css_comp_name(c), 4)); break;
        case SC_PSEUDO:
            if (c->op == PC_CHECKED)
                m |= (uint64_t)1 << HB_CHECKED;
            else if (c->op == PC_FOCUS || c->op == PC_FOCUS_VISIBLE || c->op == PC_FOCUS_WITHIN)
                m |= (uint64_t)1 << HB_FOCUS;
            else if (c->op == PC_HOVER || c->op == PC_ACTIVE)
                m |= (uint64_t)1 << HB_HOVER;
            break;
        default: break;
        }
    }
    return m;
}

static uint32_t ptr_hash(const void *p) { return (uint32_t)((uintptr_t)p >> 3) * 2654435761u; }

void css_has_index_build(CssHasIndex *x, const AuNode *root)
{
    css_has_index_free(x);
    uint32_t n = 0;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        n += c->type == AU_NODE_ELEMENT;
    uint32_t size = 16;
    while (size < n * 2)
        size *= 2;
    x->size = size;
    x->keys = au_calloc(size * sizeof *x->keys);
    x->masks = au_calloc(size * sizeof *x->masks);
    /* elements in preorder, then folded bottom-up into their parents */
    const AuElement **order = au_alloc((n ? n : 1) * sizeof *order);
    uint32_t k = 0;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        if (c->type == AU_NODE_ELEMENT)
            order[k++] = (const AuElement *)c;
    for (uint32_t i = 0; i < k; i++) {
        uint32_t h = ptr_hash(order[i]) & (size - 1);
        while (x->keys[h])
            h = (h + 1) & (size - 1);
        x->keys[h] = order[i];
    }
    for (uint32_t i = k; i-- > 0;) {
        const AuElement *el = order[i];
        const AuNode *p = el->node.parent;
        if (!p || p->type != AU_NODE_ELEMENT)
            continue;
        uint64_t sub = 0;
        uint32_t h = ptr_hash(el) & (size - 1);
        while (x->keys[h] != el)
            h = (h + 1) & (size - 1);
        sub = x->masks[h] | css_has_element_bits(el);
        h = ptr_hash(p) & (size - 1);
        while (x->keys[h] && x->keys[h] != (const AuElement *)p)
            h = (h + 1) & (size - 1);
        if (x->keys[h])
            x->masks[h] |= sub;
    }
    au_free(order);
}

void css_has_index_free(CssHasIndex *x)
{
    au_free(x->keys);
    au_free(x->masks);
    memset(x, 0, sizeof *x);
}

/* The summary of el's descendants, or all bits if el is not indexed. */
static uint64_t has_mask(const CssHasIndex *x, const AuElement *el)
{
    if (!x || !x->size)
        return ~(uint64_t)0;
    uint32_t h = ptr_hash(el) & (x->size - 1);
    while (x->keys[h]) {
        if (x->keys[h] == el)
            return x->masks[h];
        h = (h + 1) & (x->size - 1);
    }
    return ~(uint64_t)0;
}

static bool single_compound(const AuSelector *s)
{
    for (uint32_t i = 0; i < s->ncomp; i++)
        if (s->comp[i].type == SC_COMBINATOR)
            return false;
    return true;
}

static bool has_match(const AuSelList *l, const AuElement *anchor, const CssMatchCtx *mc)
{
    CssMatchCtx sub = *mc;
    sub.bloom = NULL; /* the filter describes the anchor's ancestors, not the candidates' */
    uint64_t below = ~(uint64_t)0;
    bool have_below = false;
    for (uint32_t k = 0; k < l->n; k++) {
        const AuSelector *s = &l->sel[k];
        int rc = s->rel_comb ? s->rel_comb : COMB_DESCENDANT;
        if (rc == COMB_DESCENDANT || rc == COMB_CHILD) {
            if (!have_below) {
                have_below = true;
                /* an anchor without element children needs no summary */
                bool kids = false;
                for (AuNode *n = anchor->node.first_child; n && !kids; n = n->next)
                    kids = n->type == AU_NODE_ELEMENT;
                if (!kids)
                    below = 0;
                else if (mc->has_index) {
                    if (!mc->has_index->size && mc->has_root)
                        css_has_index_build(mc->has_index, mc->has_root);
                    below = has_mask(mc->has_index, anchor);
                }
            }
            uint64_t need = css_has_selector_bits(s);
            if ((below & need) != need)
                continue;
            if (rc == COMB_CHILD && single_compound(s)) {
                /* only children can match */
                for (AuNode *n = anchor->node.first_child; n; n = n->next)
                    if (n->type == AU_NODE_ELEMENT && match_from(s, 0, (const AuElement *)n, &sub, anchor))
                        return true;
                continue;
            }
            const AuNode *root = &anchor->node;
            for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
                if (n->type != AU_NODE_ELEMENT)
                    continue;
                if (match_from(s, 0, (const AuElement *)n, &sub, anchor))
                    return true;
            }
        } else {
            for (const AuElement *sib = next_el(anchor); sib; sib = next_el(sib)) {
                if (match_from(s, 0, sib, &sub, anchor))
                    return true;
                if (rc == COMB_NEXT) {
                    /* candidates are the next sibling and, for longer
                     * selectors, its descendants */
                    const AuNode *root = &sib->node;
                    for (AuNode *n = root->first_child; n; n = au_node_next(n, root))
                        if (n->type == AU_NODE_ELEMENT && match_from(s, 0, (const AuElement *)n, &sub, anchor))
                            return true;
                    break;
                }
                const AuNode *root = &sib->node;
                for (AuNode *n = root->first_child; n; n = au_node_next(n, root))
                    if (n->type == AU_NODE_ELEMENT && match_from(s, 0, (const AuElement *)n, &sub, anchor))
                        return true;
            }
        }
    }
    return false;
}

/* A context for matching the argument of :host(), :host-context() or
 * ::slotted() against an element of the outer tree. */
static CssMatchCtx plain_ctx(const CssMatchCtx *mc)
{
    CssMatchCtx sub = *mc;
    sub.bloom = NULL;
    sub.host = NULL;
    sub.slotted = NULL;
    sub.part_el = NULL;
    sub.npart_names = 0;
    return sub;
}

static bool part_names_ok(const AuSelComp *c, const CssMatchCtx *mc)
{
    uint32_t vlen;
    const char *v = css_comp_value(c, &vlen);
    if (!v || !mc->part_el)
        return false;
    for (uint32_t i = 0; i < vlen;) {
        while (i < vlen && v[i] == ' ')
            i++;
        uint32_t s = i;
        while (i < vlen && v[i] != ' ')
            i++;
        if (i == s)
            break;
        AuAtom a = au_atom_lookup(v + s, i - s);
        bool found = false;
        for (uint32_t k = 0; a && k < mc->npart_names && !found; k++)
            found = mc->part_names[k] == a;
        if (!found)
            return false;
    }
    return true;
}

static bool match_shadow_pseudo(const AuSelComp *c, const AuElement *el, const CssMatchCtx *mc)
{
    switch (c->op) {
    case PC_HOST: {
        if (!mc->host || el != mc->host)
            return false;
        if (!c->u.list)
            return true;
        CssMatchCtx sub = plain_ctx(mc);
        return list_match(c->u.list, el, &sub);
    }
    case PC_HOST_CONTEXT: {
        if (!mc->host || el != mc->host)
            return false;
        CssMatchCtx sub = plain_ctx(mc);
        /* the host and its shadow-including ancestors */
        for (const AuNode *n = &el->node; n; n = au_shadow_including_parent(n))
            if (n->type == AU_NODE_ELEMENT && list_match(c->u.list, (const AuElement *)n, &sub))
                return true;
        return false;
    }
    case PC_SLOTTED: {
        if (!mc->slotted)
            return false;
        CssMatchCtx sub = plain_ctx(mc);
        return list_match(c->u.list, mc->slotted, &sub);
    }
    default:
        return part_names_ok(c, mc);
    }
}

static bool match_pseudo(const AuSelComp *c, const AuElement *el, const CssMatchCtx *mc)
{
    switch (c->op) {
    case PC_HOST:
    case PC_HOST_CONTEXT:
    case PC_SLOTTED:
    case PC_PART:
        return match_shadow_pseudo(c, el, mc);
    case PC_IS:
    case PC_WHERE:
        return list_match(c->u.list, el, mc);
    case PC_NOT:
        return !list_match(c->u.list, el, mc);
    case PC_HAS:
        return has_match(c->u.list, el, mc);
    case PC_NTH_CHILD:
    case PC_NTH_LAST_CHILD: {
        if (c->u.ext->u.nth.of && !list_match(c->u.ext->u.nth.of, el, mc))
            return false;
        int32_t idx = 1;
        for (const AuElement *s = c->op == PC_NTH_CHILD ? prev_el(el) : next_el(el); s;
             s = c->op == PC_NTH_CHILD ? prev_el(s) : next_el(s))
            if (!c->u.ext->u.nth.of || list_match(c->u.ext->u.nth.of, s, mc))
                idx++;
        return nth_ok(c->u.ext->u.nth.a, c->u.ext->u.nth.b, idx);
    }
    case PC_NTH_OF_TYPE:
    case PC_NTH_LAST_OF_TYPE: {
        int32_t idx = 1;
        for (const AuElement *s = c->op == PC_NTH_OF_TYPE ? prev_el(el) : next_el(el); s;
             s = c->op == PC_NTH_OF_TYPE ? prev_el(s) : next_el(s))
            if (same_type(s, el))
                idx++;
        return nth_ok(c->u.ext->u.nth.a, c->u.ext->u.nth.b, idx);
    }
    case PC_FIRST_CHILD:
        return !prev_el(el);
    case PC_LAST_CHILD:
        return !next_el(el);
    case PC_ONLY_CHILD:
        return !prev_el(el) && !next_el(el);
    case PC_FIRST_OF_TYPE:
        for (const AuElement *s = prev_el(el); s; s = prev_el(s))
            if (same_type(s, el))
                return false;
        return true;
    case PC_LAST_OF_TYPE:
        for (const AuElement *s = next_el(el); s; s = next_el(s))
            if (same_type(s, el))
                return false;
        return true;
    case PC_ONLY_OF_TYPE:
        for (const AuElement *s = prev_el(el); s; s = prev_el(s))
            if (same_type(s, el))
                return false;
        for (const AuElement *s = next_el(el); s; s = next_el(s))
            if (same_type(s, el))
                return false;
        return true;
    case PC_ROOT:
        return is_root(el);
    case PC_EMPTY:
        for (AuNode *n = el->node.first_child; n; n = n->next)
            if (n->type == AU_NODE_ELEMENT || (n->type == AU_NODE_TEXT && ((AuText *)n)->len))
                return false;
        return true;
    case PC_LINK:
    case PC_ANY_LINK:
        return is_link(el);
    case PC_VISITED:
    case PC_LOCAL_LINK:
        return false;
    case PC_HOVER:
        return (el->state & AU_STATE_HOVER) != 0;
    case PC_ACTIVE:
        return (el->state & AU_STATE_ACTIVE) != 0;
    case PC_FOCUS:
        return (el->state & AU_STATE_FOCUS) != 0;
    case PC_FOCUS_VISIBLE:
        return (el->state & AU_STATE_FOCUS_VISIBLE) != 0;
    case PC_FOCUS_WITHIN:
        return (el->state & (AU_STATE_FOCUS_WITHIN | AU_STATE_FOCUS)) != 0;
    case PC_TARGET:
        return (el->state & AU_STATE_TARGET) != 0;
    case PC_MODAL:
        return (el->state & AU_STATE_MODAL) != 0;
    case PC_POPOVER_OPEN:
        return (el->state & AU_STATE_POPOVER_OPEN) != 0;
    case PC_CHECKED:
        return is_checked(el);
    case PC_INDETERMINATE:
        if (el->state & AU_STATE_INDETERMINATE)
            return true;
        return is_html_tag(el, HN(progress)) && !attr_val(el, HN(value));
    case PC_DEFAULT:
        return (is_checkable(el) && attr_val(el, HN(checked))) ||
               (is_html_tag(el, HN(option)) && attr_val(el, HN(selected)));
    case PC_DISABLED:
        return actually_disabled(el);
    case PC_ENABLED:
        return is_form_control(el) && !actually_disabled(el);
    case PC_REQUIRED:
    case PC_OPTIONAL: {
        bool cand = is_html_tag(el, HN(input)) || is_html_tag(el, HN(select)) || is_html_tag(el, HN(textarea));
        if (!cand)
            return false;
        bool req = attr_val(el, HN(required)) != NULL;
        return c->op == PC_REQUIRED ? req : !req;
    }
    case PC_READ_WRITE:
        return read_write(el);
    case PC_READ_ONLY:
        return !read_write(el);
    case PC_PLACEHOLDER_SHOWN: {
        if (!is_text_input(el) && !is_html_tag(el, HN(textarea)))
            return false;
        const char *ph = attr_val(el, HN(placeholder));
        return ph && *ph && value_empty(el);
    }
    case PC_DEFINED:
        if (!is_html(el))
            return true;
        if (strchr(au_atom_str(el->tag), '-'))
            return (el->state & AU_STATE_DEFINED) != 0;
        return true;
    case PC_LANG:
        return lang_matches(el, c->u.ext->u.value.s, c->u.ext->u.value.len);
    case PC_DIR:
        return directionality(el) == (int)c->u.name;
    case PC_SCOPE:
        return mc->scope ? el == mc->scope : is_root(el);
    case PC_VALID:
        return validation_candidate(el) && !constraint_invalid(el);
    case PC_INVALID:
        return validation_candidate(el) && constraint_invalid(el);
    case PC_USER_VALID:
    case PC_USER_INVALID:
        return false;
    case PC_IN_RANGE:
    case PC_OUT_OF_RANGE: {
        bool in;
        if (!range_state(el, &in))
            return false;
        return c->op == PC_IN_RANGE ? in : !in;
    }
    case PC_NEVER:
    default:
        return false;
    }
}

static bool ns_ok(const AuSelComp *c, const AuElement *el)
{
    if (c->flags & SCF_NS_ANY)
        return true;
    if (c->flags & SCF_NS_NONE)
        return false; /* every element here has a namespace */
    if (c->flags & SCF_NS_SET)
        return c->ns != 255 && c->ns == el->ns;
    return true;
}

static bool match_simple(const AuSelComp *c, const AuElement *el, const CssMatchCtx *mc)
{
    switch (c->type) {
    case SC_TAG:
        if (!ns_ok(c, el))
            return false;
        return el->tag == (is_html(el) ? css_comp_name(c) : css_comp_name_cs(c));
    case SC_UNIVERSAL:
        return ns_ok(c, el);
    case SC_ID:
        if (!el->id)
            return false;
        if (el->id == c->u.name)
            return true;
        return mc->quirks && css_atom_lower(el->id) == css_atom_lower(c->u.name);
    case SC_CLASS:
        if (au_element_has_class(el, c->u.name))
            return true;
        if (mc->quirks) {
            AuAtom lc = css_atom_lower(c->u.name);
            for (uint32_t i = 0; i < el->nclasses; i++)
                if (css_atom_lower(el->classes[i]) == lc)
                    return true;
        }
        return false;
    case SC_ATTR:
        return match_attr(c, el);
    case SC_PSEUDO:
        if (c->flags & SCF_ON_PART) {
            if (!mc->part_el)
                return false;
            CssMatchCtx sub = plain_ctx(mc);
            return match_pseudo(c, mc->part_el, &sub);
        }
        return match_pseudo(c, el, mc);
    case SC_NESTING:
        if (!c->u.list)
            return mc->scope ? el == mc->scope : is_root(el);
        return list_match(c->u.list, el, mc);
    default:
        return false;
    }
}

static bool anchor_rel(const AuElement *el, const AuElement *anchor, int comb)
{
    switch (comb) {
    case COMB_CHILD:
        return parent_el(el) == anchor;
    case COMB_NEXT:
        return prev_el(el) == anchor;
    case COMB_SUBSEQUENT:
        for (const AuElement *s = prev_el(el); s; s = prev_el(s))
            if (s == anchor)
                return true;
        return false;
    default:
        for (const AuElement *p = parent_el(el); p; p = parent_el(p))
            if (p == anchor)
                return true;
        return false;
    }
}

/* How a match of the rest of a selector failed. Without it a chain of
 * descendant combinators on a deep tree backtracks without limit ("p div
 * div div ... div" over 40 nested divs tries every choice of ancestors):
 * when the rest of the selector fails because it ran out of ancestors, no
 * ancestor further up can do better, and likewise for siblings. */
typedef enum {
    M_MATCH,
    M_FAIL,          /* this element fails; another candidate may match */
    M_FAIL_SIBLINGS, /* no earlier sibling of this element can match */
    M_FAIL_ALL       /* no sibling and no ancestor can match */
} MatchResult;

/* What the shadow host of a tree may match from inside it (it is
 * featureless there): :host, :host(), :host-context(), and :is() or
 * :where() of those; a ::part() rule's own components apply to others. */
static bool host_comp_ok(const AuSelComp *c)
{
    if (c->type == SC_UNIVERSAL)
        return true;
    if (c->type != SC_PSEUDO)
        return false;
    if (c->flags & SCF_ON_PART)
        return true;
    return c->op == PC_HOST || c->op == PC_HOST_CONTEXT || c->op == PC_IS || c->op == PC_WHERE ||
           c->op == PC_PART;
}

/* The element a descendant or child combinator steps to: the parent,
 * or, from the top of the shadow tree whose rules are matched, its host.
 * Nothing is above the host in that tree. */
static const AuElement *m_parent(const AuElement *el, const CssMatchCtx *mc)
{
    if (mc->host) {
        if (el == mc->host)
            return NULL;
        AuNode *p = el->node.parent;
        if (p && au_is_shadow_root(p) && ((const AuShadowRoot *)p)->host == mc->host)
            return mc->host;
    }
    return parent_el(el);
}

static MatchResult match_rest(const AuSelector *s, uint32_t i, const AuElement *el, const CssMatchCtx *mc,
                              const AuElement *anchor)
{
    uint32_t k = i;
    bool featureless = mc->host && el == mc->host;
    bool hosted = false; /* the featureless host needs a :host-like component (a lone * fails) */
    for (; k < s->ncomp && s->comp[k].type != SC_COMBINATOR; k++) {
        if (featureless) {
            if (!host_comp_ok(&s->comp[k]))
                return M_FAIL;
            hosted |= s->comp[k].type == SC_PSEUDO;
        }
        if (!match_simple(&s->comp[k], el, mc))
            return M_FAIL;
    }
    if (featureless && !hosted)
        return M_FAIL;
    if (k >= s->ncomp) {
        if (anchor)
            return anchor_rel(el, anchor, s->rel_comb ? s->rel_comb : COMB_DESCENDANT) ? M_MATCH : M_FAIL;
        return M_MATCH;
    }
    int comb = s->comp[k].op;
    MatchResult r;
    k++;
    switch (comb) {
    case COMB_DESCENDANT:
        for (const AuElement *p = m_parent(el, mc); p; p = m_parent(p, mc)) {
            if (anchor && p == anchor)
                return M_FAIL_ALL;
            r = match_rest(s, k, p, mc, anchor);
            if (r == M_MATCH || r == M_FAIL_ALL)
                return r;
        }
        return M_FAIL_ALL;
    case COMB_CHILD: {
        const AuElement *p = m_parent(el, mc);
        if (!p || (anchor && p == anchor))
            return M_FAIL_ALL;
        r = match_rest(s, k, p, mc, anchor);
        return r == M_MATCH || r == M_FAIL_ALL ? r : M_FAIL_SIBLINGS;
    }
    case COMB_NEXT: {
        if (mc->host && el == mc->host)
            return M_FAIL_ALL; /* the host has no siblings in its shadow tree */
        const AuElement *p = prev_el(el);
        if (!p || p == anchor)
            return M_FAIL_SIBLINGS;
        return match_rest(s, k, p, mc, anchor);
    }
    case COMB_SUBSEQUENT:
        if (mc->host && el == mc->host)
            return M_FAIL_ALL;
        for (const AuElement *p = prev_el(el); p; p = prev_el(p)) {
            if (p == anchor)
                return M_FAIL_SIBLINGS;
            r = match_rest(s, k, p, mc, anchor);
            if (r != M_FAIL)
                return r;
        }
        return M_FAIL_SIBLINGS;
    default:
        return M_FAIL;
    }
}

static bool match_from(const AuSelector *s, uint32_t i, const AuElement *el, const CssMatchCtx *mc,
                       const AuElement *anchor)
{
    return match_rest(s, i, el, mc, anchor) == M_MATCH;
}

bool css_selector_match(const AuSelector *s, const AuElement *el, const CssMatchCtx *mc)
{
    if (mc->bloom) {
        const uint32_t *h = css_sel_hashes(s);
        for (uint32_t i = 0; i < s->nhash; i++)
            if (!css_bloom_may_contain(mc->bloom, h[i]))
                return false;
    }
    return match_from(s, 0, el, mc, NULL);
}

bool css_sellist_match(const AuSelList *l, const AuElement *el, const CssMatchCtx *mc)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (css_selector_match(&l->sel[i], el, mc))
            return true;
    return false;
}

/* attr() for content: the attribute value, or NULL. */
const char *css_attr_value(const AuElement *el, const char *name, size_t n)
{
    char buf[64];
    AuAtom a;
    if (is_html(el) && css_lower(name, n, buf, sizeof buf))
        a = au_atom_lookup(buf, n);
    else
        a = au_atom_lookup(name, n);
    if (!a)
        return NULL;
    return au_element_get_attr(el, a);
}
