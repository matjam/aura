/* Forms: owners, options, the entry list, encodings and validation (see
 * formdata.h). */
#include "formdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *get_attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static bool is_html(const AuNode *n, AuAtom tag)
{
    return au_is_html_element(n, tag);
}

static AuElement *parent_el(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_ELEMENT ? au_as_element(p) : NULL;
}

/* ---- Elements ---- */

bool au_form_is_listed(const AuElement *el)
{
    if (el->ns != AU_NS_HTML)
        return false;
    AuAtom t = el->tag;
    return t == AU_ATOM_button || t == AU_ATOM_fieldset || t == AU_ATOM_input || t == AU_ATOM_object ||
           t == AU_ATOM_output || t == AU_ATOM_select || t == AU_ATOM_textarea;
}

bool au_form_is_submittable(const AuElement *el)
{
    if (el->ns != AU_NS_HTML)
        return false;
    AuAtom t = el->tag;
    return t == AU_ATOM_button || t == AU_ATOM_input || t == AU_ATOM_select || t == AU_ATOM_textarea;
}

AuElement *au_form_owner(AuElement *el)
{
    const char *id = get_attr(el, "form");
    if (id) {
        if (!au_node_is_connected(&el->node))
            return NULL;
        AuNode *root = au_node_root(&el->node);
        AuAtom a = au_atom_lookup(id, strlen(id));
        if (!a)
            return NULL;
        for (AuNode *n = root; n; n = au_node_next(n, root))
            if (n->type == AU_NODE_ELEMENT && au_as_element(n)->id == a)
                return is_html(n, AU_ATOM_form) ? au_as_element(n) : NULL;
        return NULL;
    }
    for (AuElement *p = parent_el(el); p; p = parent_el(p))
        if (is_html(&p->node, AU_ATOM_form))
            return p;
    return NULL;
}

bool au_form_disabled(const AuElement *el)
{
    if (au_element_get_attr(el, AU_ATOM_disabled))
        return true;
    if (is_html(&el->node, AU_ATOM_option)) {
        const AuElement *p = parent_el(el);
        return p && is_html(&p->node, AU_ATOM_optgroup) && au_element_get_attr(p, AU_ATOM_disabled);
    }
    if (is_html(&el->node, AU_ATOM_optgroup))
        return false;
    const AuElement *child = el;
    for (const AuElement *p = parent_el(el); p; child = p, p = parent_el(p)) {
        if (is_html(&p->node, AU_ATOM_fieldset) && au_element_get_attr(p, AU_ATOM_disabled)) {
            const AuNode *legend = NULL;
            for (AuNode *c = p->node.first_child; c; c = c->next)
                if (is_html(c, AU_ATOM_legend)) {
                    legend = c;
                    break;
                }
            if (&child->node != legend)
                return true;
        }
    }
    return false;
}

typedef struct ElList {
    AuElement **e;
    size_t n, cap;
} ElList;

static void push(ElList *l, AuElement *el)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->e = au_realloc(l->e, l->cap * sizeof *l->e);
    }
    l->e[l->n++] = el;
}

AuElement **au_form_elements(AuElement *form, size_t *n)
{
    ElList l = {0};
    AuNode *root = au_node_root(&form->node);
    for (AuNode *c = root; c; c = au_node_next(c, root)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *e = au_as_element(c);
        if (au_form_is_listed(e) && au_form_owner(e) == form)
            push(&l, e);
    }
    *n = l.n;
    return l.e;
}

bool au_form_is_submit_button(const AuElement *el)
{
    if (is_html(&el->node, AU_ATOM_button)) {
        const char *t = au_element_get_attr(el, AU_ATOM_type);
        return !t || au_ieq(t, strlen(t), "submit", 6) ||
               (!au_ieq(t, strlen(t), "reset", 5) && !au_ieq(t, strlen(t), "button", 6));
    }
    if (is_html(&el->node, AU_ATOM_input)) {
        AuInputType t = au_input_type(el);
        return t == AU_INPUT_SUBMIT || t == AU_INPUT_IMAGE;
    }
    return false;
}

AuElement *au_form_default_button(AuElement *form)
{
    size_t n;
    AuElement **els = au_form_elements(form, &n);
    AuElement *r = NULL;
    for (size_t i = 0; i < n && !r; i++)
        if (au_form_is_submit_button(els[i]))
            r = els[i];
    au_free(els);
    return r;
}

AuElement **au_form_radio_group(AuElement *el, size_t *n)
{
    ElList l = {0};
    const char *name = get_attr(el, "name");
    if (!name || !*name) {
        push(&l, el);
        *n = l.n;
        return l.e;
    }
    AuElement *form = au_form_owner(el);
    AuNode *root = au_node_root(&el->node);
    for (AuNode *c = root; c; c = au_node_next(c, root)) {
        if (!is_html(c, AU_ATOM_input))
            continue;
        AuElement *e = au_as_element(c);
        if (au_input_type(e) != AU_INPUT_RADIO)
            continue;
        const char *nm = get_attr(e, "name");
        if (nm && !strcmp(nm, name) && au_form_owner(e) == form)
            push(&l, e);
    }
    *n = l.n;
    return l.e;
}

/* ---- Select ---- */

AuElement **au_select_options(AuElement *sel, bool groups, size_t *n)
{
    ElList l = {0};
    for (AuNode *c = sel->node.first_child; c; c = c->next) {
        if (is_html(c, AU_ATOM_option)) {
            push(&l, au_as_element(c));
        } else if (is_html(c, AU_ATOM_optgroup)) {
            if (groups)
                push(&l, au_as_element(c));
            for (AuNode *o = c->first_child; o; o = o->next)
                if (is_html(o, AU_ATOM_option))
                    push(&l, au_as_element(o));
        }
    }
    *n = l.n;
    return l.e;
}

bool au_select_is_listbox(const AuElement *sel)
{
    if (au_element_get_attr(sel, AU_ATOM_multiple))
        return true;
    const char *sz = au_element_get_attr(sel, AU_ATOM_size);
    return sz && atoi(sz) > 1;
}

int au_select_index(AuElement *sel)
{
    size_t n;
    AuElement **o = au_select_options(sel, false, &n);
    int r = -1;
    bool multi = au_select_is_listbox(sel);
    for (size_t i = 0; i < n; i++)
        if (au_ctl_checked(o[i])) {
            r = (int)i;
            if (multi)
                break; /* the first; a drop-down shows its last */
        }
    if (r < 0 && !multi)
        for (size_t i = 0; i < n && r < 0; i++)
            if (!au_form_disabled(o[i]))
                r = (int)i;
    au_free(o);
    return r;
}

void au_select_set_index(AuElement *sel, int idx)
{
    size_t n;
    AuElement **o = au_select_options(sel, false, &n);
    for (size_t i = 0; i < n; i++)
        au_ctl_set_checked(o[i], (int)i == idx);
    au_free(o);
}

static void collapsed_text(const AuNode *n, AuBuf *out)
{
    bool sp = true;
    size_t start = out->len;
    for (AuNode *c = au_node_next(n, n); c; c = au_node_next(c, n)) {
        if (c->type != AU_NODE_TEXT)
            continue;
        AuText *t = au_as_text(c);
        for (size_t i = 0; i < t->len; i++) {
            char ch = t->data[i];
            if (au_is_space((unsigned char)ch)) {
                if (!sp)
                    au_buf_putc(out, ' ');
                sp = true;
            } else {
                au_buf_putc(out, ch);
                sp = false;
            }
        }
    }
    while (out->len > start && out->data[out->len - 1] == ' ')
        out->len--;
}

void au_option_value(const AuElement *opt, AuBuf *out)
{
    const char *v = get_attr(opt, "value");
    if (v)
        au_buf_puts(out, v);
    else
        collapsed_text(&opt->node, out);
}

void au_option_label(const AuElement *opt, AuBuf *out)
{
    const char *v = get_attr(opt, "label");
    if (v && *v)
        au_buf_puts(out, v);
    else
        collapsed_text(&opt->node, out);
}

/* ---- The entry list ---- */

void au_form_entries_init(AuFormEntries *l)
{
    memset(l, 0, sizeof *l);
}

void au_form_entries_free(AuFormEntries *l)
{
    for (size_t i = 0; i < l->n; i++) {
        au_free(l->e[i].name);
        au_free(l->e[i].value);
        au_free(l->e[i].filename);
        au_free(l->e[i].type);
    }
    au_free(l->e);
    memset(l, 0, sizeof *l);
}

static AuFormEntry *add_entry(AuFormEntries *l, const char *name)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->e = au_realloc(l->e, l->cap * sizeof *l->e);
    }
    AuFormEntry *e = &l->e[l->n++];
    memset(e, 0, sizeof *e);
    e->name = au_strndup(name, strlen(name));
    return e;
}

void au_form_entries_add(AuFormEntries *l, const char *name, const char *value, size_t len)
{
    AuFormEntry *e = add_entry(l, name);
    e->value = au_strndup(value ? value : "", value ? len : 0);
    e->value_len = value ? len : 0;
}

void au_form_entries_add_file(AuFormEntries *l, const char *name, const char *filename, const char *type,
                              const void *data, size_t len)
{
    AuFormEntry *e = add_entry(l, name);
    e->file = true;
    e->value = au_alloc(len + 1);
    if (len)
        memcpy(e->value, data, len);
    e->value[len] = 0;
    e->value_len = len;
    e->filename = au_strndup(filename ? filename : "", filename ? strlen(filename) : 0);
    const char *t = type && *type ? type : "application/octet-stream";
    e->type = au_strndup(t, strlen(t));
}

static bool in_datalist(const AuElement *el)
{
    for (const AuElement *p = parent_el(el); p; p = parent_el(p))
        if (is_html(&p->node, AU_ATOM_datalist))
            return true;
    return false;
}

void au_form_entries_build(AuElement *form, AuElement *submitter, int image_x, int image_y, AuFormEntries *out)
{
    size_t n;
    AuElement **els = au_form_elements(form, &n);
    AuBuf tmp;
    au_buf_init(&tmp);
    for (size_t i = 0; i < n; i++) {
        AuElement *e = els[i];
        if (!au_form_is_submittable(e) || in_datalist(e) || au_form_disabled(e))
            continue;
        bool input = is_html(&e->node, AU_ATOM_input);
        AuInputType t = input ? au_input_type(e) : AU_INPUT_TEXT;
        bool button = is_html(&e->node, AU_ATOM_button) ||
                      (input && (t == AU_INPUT_SUBMIT || t == AU_INPUT_RESET || t == AU_INPUT_BUTTON ||
                                 t == AU_INPUT_IMAGE));
        if (button && e != submitter)
            continue;
        if (input && (t == AU_INPUT_CHECKBOX || t == AU_INPUT_RADIO) && !au_ctl_checked(e))
            continue;
        const char *name = get_attr(e, "name");
        if (input && t == AU_INPUT_IMAGE) {
            char key[512], num[32];
            const char *pre = name && *name ? name : "";
            snprintf(key, sizeof key, "%s%sx", pre, *pre ? "." : "");
            snprintf(num, sizeof num, "%d", image_x);
            au_form_entries_add(out, key, num, strlen(num));
            snprintf(key, sizeof key, "%s%sy", pre, *pre ? "." : "");
            snprintf(num, sizeof num, "%d", image_y);
            au_form_entries_add(out, key, num, strlen(num));
            continue;
        }
        if (!name || !*name)
            continue;
        if (is_html(&e->node, AU_ATOM_select)) {
            size_t no;
            AuElement **opts = au_select_options(e, false, &no);
            bool multi = au_select_is_listbox(e);
            int one = multi ? -1 : au_select_index(e);
            for (size_t k = 0; k < no; k++) {
                bool sel = multi ? au_ctl_checked(opts[k]) : (int)k == one;
                if (!sel || au_form_disabled(opts[k]))
                    continue;
                au_buf_clear(&tmp);
                au_option_value(opts[k], &tmp);
                au_form_entries_add(out, name, tmp.data ? tmp.data : "", tmp.len);
            }
            au_free(opts);
            continue;
        }
        if (input && (t == AU_INPUT_CHECKBOX || t == AU_INPUT_RADIO)) {
            const char *v = get_attr(e, "value");
            if (!v)
                v = "on";
            au_form_entries_add(out, name, v, strlen(v));
            continue;
        }
        if (input && t == AU_INPUT_FILE) {
            AuControl *c = au_ctl_peek(e);
            if (!c || !c->nfiles)
                au_form_entries_add_file(out, name, "", "application/octet-stream", "", 0);
            else
                for (uint32_t k = 0; k < c->nfiles; k++)
                    au_form_entries_add_file(out, name, c->files[k].name, c->files[k].type, c->files[k].data,
                                             c->files[k].len);
            continue;
        }
        if (input && t == AU_INPUT_HIDDEN && au_ieq(name, strlen(name), "_charset_", 9)) {
            au_form_entries_add(out, name, "UTF-8", 5);
            continue;
        }
        size_t vn;
        const char *v;
        if (button) {
            v = get_attr(e, "value");
            if (!v)
                v = "";
            vn = strlen(v);
        } else {
            v = au_ctl_value(e, &vn);
        }
        au_form_entries_add(out, name, v, vn);
        const char *dir = get_attr(e, "dirname");
        if (dir && *dir && (is_html(&e->node, AU_ATOM_textarea) || (input && (t == AU_INPUT_TEXT || t == AU_INPUT_SEARCH))))
            au_form_entries_add(out, dir, "ltr", 3);
    }
    au_buf_free(&tmp);
    au_free(els);
}

/* ---- Encodings ---- */

/* Appends s with every CR, LF and CR LF made CR LF. */
static void put_crlf(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' || s[i] == '\n') {
            au_buf_append(out, "\r\n", 2);
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
                i++;
        } else {
            au_buf_putc(out, s[i]);
        }
    }
}

void au_form_urlencode_string(const char *s, size_t n, AuBuf *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (au_is_alpha(c) || au_is_digit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            au_buf_putc(out, (char)c);
        } else if (c == ' ') {
            au_buf_putc(out, '+');
        } else {
            au_buf_putc(out, '%');
            au_buf_putc(out, hex[c >> 4]);
            au_buf_putc(out, hex[c & 15]);
        }
    }
}

void au_form_urlencode(const AuFormEntries *l, AuBuf *out)
{
    AuBuf t;
    au_buf_init(&t);
    for (size_t i = 0; i < l->n; i++) {
        const AuFormEntry *e = &l->e[i];
        if (i)
            au_buf_putc(out, '&');
        au_buf_clear(&t);
        put_crlf(&t, e->name, strlen(e->name));
        au_form_urlencode_string(t.data ? t.data : "", t.len, out);
        au_buf_putc(out, '=');
        au_buf_clear(&t);
        /* A file is sent as its name. */
        if (e->file)
            au_buf_puts(&t, e->filename);
        else
            put_crlf(&t, e->value, e->value_len);
        au_form_urlencode_string(t.data ? t.data : "", t.len, out);
    }
    au_buf_free(&t);
}

/* A name or filename in a multipart header: LF, CR and '"' escaped. */
static void put_mp_name(AuBuf *out, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            au_buf_puts(out, "%0A");
        else if (*s == '\r')
            au_buf_puts(out, "%0D");
        else if (*s == '"')
            au_buf_puts(out, "%22");
        else
            au_buf_putc(out, *s);
    }
}

void au_form_multipart(const AuFormEntries *l, const char *boundary, AuBuf *out)
{
    AuBuf t;
    au_buf_init(&t);
    for (size_t i = 0; i < l->n; i++) {
        const AuFormEntry *e = &l->e[i];
        au_buf_puts(out, "--");
        au_buf_puts(out, boundary);
        au_buf_puts(out, "\r\nContent-Disposition: form-data; name=\"");
        au_buf_clear(&t);
        if (!e->file)
            put_crlf(&t, e->name, strlen(e->name));
        else
            au_buf_puts(&t, e->name);
        put_mp_name(out, au_buf_cstr(&t));
        au_buf_putc(out, '"');
        if (e->file) {
            au_buf_puts(out, "; filename=\"");
            put_mp_name(out, e->filename);
            au_buf_puts(out, "\"\r\nContent-Type: ");
            au_buf_puts(out, e->type);
            au_buf_puts(out, "\r\n\r\n");
            au_buf_append(out, e->value, e->value_len);
        } else {
            au_buf_puts(out, "\r\n\r\n");
            put_crlf(out, e->value, e->value_len);
        }
        au_buf_puts(out, "\r\n");
    }
    au_buf_puts(out, "--");
    au_buf_puts(out, boundary);
    au_buf_puts(out, "--\r\n");
    au_buf_free(&t);
}

void au_form_text_plain(const AuFormEntries *l, AuBuf *out)
{
    for (size_t i = 0; i < l->n; i++) {
        const AuFormEntry *e = &l->e[i];
        put_crlf(out, e->name, strlen(e->name));
        au_buf_putc(out, '=');
        if (e->file)
            au_buf_puts(out, e->filename);
        else
            put_crlf(out, e->value, e->value_len);
        au_buf_puts(out, "\r\n");
    }
}

/* ---- Validation ---- */

bool au_form_will_validate(AuElement *el)
{
    if (!au_form_is_submittable(el) || au_form_disabled(el) || in_datalist(el))
        return false;
    if (is_html(&el->node, AU_ATOM_button))
        return au_form_is_submit_button(el);
    if (is_html(&el->node, AU_ATOM_input)) {
        AuInputType t = au_input_type(el);
        if (t == AU_INPUT_HIDDEN || t == AU_INPUT_RESET || t == AU_INPUT_BUTTON)
            return false;
        if (au_input_type_is_text(t) && get_attr(el, "readonly"))
            return false;
    }
    if (is_html(&el->node, AU_ATOM_textarea) && get_attr(el, "readonly"))
        return false;
    return true;
}

static bool valid_email(const char *s, size_t n)
{
    /* one or more addresses for multiple; checked as one: local@domain */
    const char *at = memchr(s, '@', n);
    if (!at || at == s || at == s + n - 1)
        return false;
    for (const char *p = s; p < s + n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == '<' || c == '>' || c == '(' || c == ')' || c == ',' || c == ';' || c == ':' || c == '"' ||
            c == '[' || c == ']' || c == '\\')
            return false;
        if (c == '@' && p != at)
            return false;
    }
    const char *d = at + 1;
    size_t dn = (size_t)(s + n - d);
    if (d[0] == '.' || d[dn - 1] == '.' || d[0] == '-')
        return false;
    return true;
}

static bool valid_absolute_url(const char *s, size_t n)
{
    /* scheme ":" rest, the scheme an ASCII letter then letters, digits, + - . */
    if (!n || !au_is_alpha((unsigned char)s[0]))
        return false;
    size_t i = 1;
    while (i < n && (au_is_alpha((unsigned char)s[i]) || au_is_digit((unsigned char)s[i]) || s[i] == '+' ||
                     s[i] == '-' || s[i] == '.'))
        i++;
    if (i >= n || s[i] != ':')
        return false;
    for (size_t k = 0; k < n; k++)
        if ((unsigned char)s[k] <= ' ')
            return false;
    return true;
}

static bool parse_num(const char *v, double *out)
{
    if (!v || !*v)
        return false;
    char *end;
    double d = strtod(v, &end);
    if (*end)
        return false;
    *out = d;
    return true;
}

unsigned au_form_validity(AuElement *el, AuPatternTest test, void *ctx)
{
    if (!au_form_will_validate(el))
        return 0;
    unsigned r = 0;
    AuControl *c = au_ctl_peek(el);
    if (c && c->custom && *c->custom)
        r |= AU_VALID_CUSTOM;
    bool input = is_html(&el->node, AU_ATOM_input);
    bool required = get_attr(el, "required") != NULL;
    AuInputType t = input ? au_input_type(el) : AU_INPUT_TEXT;
    if (is_html(&el->node, AU_ATOM_select)) {
        if (required) {
            /* Missing: no selected option, or the placeholder option (the
             * first, with an empty value, of a drop-down) chosen. */
            size_t n;
            AuElement **o = au_select_options(el, false, &n);
            int idx = au_select_index(el);
            bool any = false;
            if (au_select_is_listbox(el)) {
                for (size_t i = 0; i < n; i++)
                    any |= au_ctl_checked(o[i]);
            } else if (idx >= 0) {
                AuBuf v;
                au_buf_init(&v);
                au_option_value(o[idx], &v);
                any = !(idx == 0 && v.len == 0 && o[0]->node.parent == &el->node);
                au_buf_free(&v);
            }
            if (!any)
                r |= AU_VALID_MISSING;
            au_free(o);
        }
        return r;
    }
    if (input && (t == AU_INPUT_CHECKBOX || t == AU_INPUT_RADIO)) {
        if (!required && t == AU_INPUT_RADIO) {
            /* any required member makes the group required */
            size_t n;
            AuElement **g = au_form_radio_group(el, &n);
            for (size_t i = 0; i < n; i++)
                required |= get_attr(g[i], "required") != NULL;
            au_free(g);
        }
        if (required) {
            bool on = au_ctl_checked(el);
            if (t == AU_INPUT_RADIO && !on) {
                size_t n;
                AuElement **g = au_form_radio_group(el, &n);
                for (size_t i = 0; i < n; i++)
                    on |= au_ctl_checked(g[i]);
                au_free(g);
            }
            if (!on)
                r |= AU_VALID_MISSING;
        }
        return r;
    }
    if (input && t == AU_INPUT_FILE) {
        if (required && (!c || !c->nfiles))
            r |= AU_VALID_MISSING;
        return r;
    }
    if (!(input || is_html(&el->node, AU_ATOM_textarea)))
        return r;
    if (input && (t == AU_INPUT_SUBMIT || t == AU_INPUT_IMAGE || t == AU_INPUT_COLOR))
        return r;
    size_t vn;
    const char *v = au_ctl_value(el, &vn);
    if (required && vn == 0 && t != AU_INPUT_RANGE)
        r |= AU_VALID_MISSING;
    if (vn == 0)
        return r;
    if (t == AU_INPUT_EMAIL && !valid_email(v, vn))
        r |= AU_VALID_TYPE;
    if (t == AU_INPUT_URL && !valid_absolute_url(v, vn))
        r |= AU_VALID_TYPE;
    const char *pat = get_attr(el, "pattern");
    if (pat && test && (!input || au_input_type_is_text(t)) && test(ctx, pat, v) == 0)
        r |= AU_VALID_PATTERN;
    /* maxlength and minlength apply after the user edited the value */
    if (c && c->dirty && c->user_interacted) {
        size_t chars = au_utf8_count(v, vn);
        const char *mx = get_attr(el, "maxlength"), *mn = get_attr(el, "minlength");
        if (mx && atoi(mx) >= 0 && au_is_digit((unsigned char)*mx) && chars > (size_t)atoi(mx))
            r |= AU_VALID_TOO_LONG;
        if (mn && au_is_digit((unsigned char)*mn) && chars < (size_t)atoi(mn))
            r |= AU_VALID_TOO_SHORT;
    }
    if (t == AU_INPUT_NUMBER || t == AU_INPUT_RANGE) {
        double x, lim;
        if (parse_num(v, &x)) {
            if (parse_num(get_attr(el, "min"), &lim) && x < lim)
                r |= AU_VALID_UNDERFLOW;
            if (parse_num(get_attr(el, "max"), &lim) && x > lim)
                r |= AU_VALID_OVERFLOW;
        }
    }
    return r;
}

const char *au_form_validity_message(AuElement *el, unsigned bits)
{
    AuControl *c = au_ctl_peek(el);
    if ((bits & AU_VALID_CUSTOM) && c && c->custom)
        return c->custom;
    bool input = is_html(&el->node, AU_ATOM_input);
    AuInputType t = input ? au_input_type(el) : AU_INPUT_TEXT;
    if (bits & AU_VALID_MISSING) {
        if (t == AU_INPUT_CHECKBOX && input)
            return "Please check this box if you want to proceed.";
        if (t == AU_INPUT_RADIO && input)
            return "Please select one of these options.";
        if (is_html(&el->node, AU_ATOM_select))
            return "Please select an item in the list.";
        if (t == AU_INPUT_FILE && input)
            return "Please select a file.";
        return "Please fill out this field.";
    }
    if (bits & AU_VALID_TYPE)
        return t == AU_INPUT_EMAIL ? "Please enter an email address." : "Please enter a URL.";
    if (bits & AU_VALID_PATTERN) {
        const char *title = get_attr(el, "title");
        return title && *title ? title : "Please match the requested format.";
    }
    if (bits & AU_VALID_TOO_LONG)
        return "Please shorten this text.";
    if (bits & AU_VALID_TOO_SHORT)
        return "Please lengthen this text.";
    if (bits & AU_VALID_UNDERFLOW)
        return "Value is too small.";
    if (bits & AU_VALID_OVERFLOW)
        return "Value is too large.";
    return bits ? "Please enter a valid value." : "";
}
