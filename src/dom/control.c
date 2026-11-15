/* Form control state (see control.h). */
#include "control.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *get_attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static bool is_html(const AuElement *el, AuAtom tag)
{
    return el->ns == AU_NS_HTML && el->tag == tag;
}

AuInputType au_input_type(const AuElement *el)
{
    static const struct {
        const char *name;
        AuInputType t;
    } types[] = {
        {"text", AU_INPUT_TEXT},         {"search", AU_INPUT_SEARCH},     {"tel", AU_INPUT_TEL},
        {"url", AU_INPUT_URL},           {"email", AU_INPUT_EMAIL},       {"password", AU_INPUT_PASSWORD},
        {"number", AU_INPUT_NUMBER},     {"date", AU_INPUT_DATE},         {"month", AU_INPUT_DATE},
        {"week", AU_INPUT_DATE},         {"time", AU_INPUT_DATE},         {"datetime-local", AU_INPUT_DATE},
        {"hidden", AU_INPUT_HIDDEN},     {"checkbox", AU_INPUT_CHECKBOX}, {"radio", AU_INPUT_RADIO},
        {"file", AU_INPUT_FILE},         {"submit", AU_INPUT_SUBMIT},     {"image", AU_INPUT_IMAGE},
        {"reset", AU_INPUT_RESET},       {"button", AU_INPUT_BUTTON},     {"range", AU_INPUT_RANGE},
        {"color", AU_INPUT_COLOR},
    };
    const char *v = au_element_get_attr(el, AU_ATOM_type);
    if (!v)
        return AU_INPUT_TEXT;
    size_t n = strlen(v);
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (au_ieq(v, n, types[i].name, strlen(types[i].name)))
            return types[i].t;
    return AU_INPUT_TEXT;
}

bool au_input_type_is_text(AuInputType t)
{
    return t <= AU_INPUT_DATE;
}

bool au_ctl_is_single_line(const AuElement *el)
{
    return is_html(el, AU_ATOM_input) && au_input_type_is_text(au_input_type(el));
}

bool au_ctl_is_text_field(const AuElement *el)
{
    return is_html(el, AU_ATOM_textarea) || au_ctl_is_single_line(el);
}

AuControl *au_ctl(AuElement *el)
{
    if (!el->control) {
        AuControl *c = au_calloc(sizeof *c);
        au_buf_init(&c->value);
        au_buf_init(&c->undo);
        c->cursor = -1;
        el->control = c;
    }
    return el->control;
}

static void free_files(AuControl *c)
{
    for (uint32_t i = 0; i < c->nfiles; i++) {
        au_free(c->files[i].name);
        au_free(c->files[i].type);
        au_free(c->files[i].data);
    }
    au_free(c->files);
    c->files = NULL;
    c->nfiles = 0;
}

void au_ctl_free(AuElement *el)
{
    AuControl *c = el->control;
    if (!c)
        return;
    el->control = NULL;
    au_buf_free(&c->value);
    au_buf_free(&c->undo);
    au_free(c->focus_value);
    au_free(c->custom);
    free_files(c);
    au_free(c);
}

size_t au_utf8_count(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        n += ((unsigned char)s[i] & 0xC0) != 0x80;
    return n;
}

/* ---- Sanitization ---- */

static bool valid_float(const char *s, size_t n)
{
    /* HTML "valid floating-point number": -?(\d+(\.\d+)?|\.\d+)([eE][+-]?\d+)? */
    size_t i = 0;
    if (i < n && s[i] == '-')
        i++;
    size_t d0 = i;
    while (i < n && au_is_digit((unsigned char)s[i]))
        i++;
    bool intpart = i > d0;
    if (i < n && s[i] == '.') {
        i++;
        size_t f0 = i;
        while (i < n && au_is_digit((unsigned char)s[i]))
            i++;
        if (i == f0)
            return false;
    } else if (!intpart) {
        return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        size_t e0 = i;
        while (i < n && au_is_digit((unsigned char)s[i]))
            i++;
        if (i == e0)
            return false;
    }
    return i == n;
}

static double num_attr(const AuElement *el, const char *name, double dflt)
{
    const char *v = get_attr(el, name);
    if (!v || !valid_float(v, strlen(v)))
        return dflt;
    return strtod(v, NULL);
}

static void put_number(AuBuf *out, double v)
{
    char tmp[64];
    if (v == floor(v) && fabs(v) < 1e15)
        snprintf(tmp, sizeof tmp, "%.0f", v);
    else
        snprintf(tmp, sizeof tmp, "%.10g", v);
    au_buf_puts(out, tmp);
}

void au_ctl_sanitize(AuElement *el, AuInputType t, const char *v, size_t len, AuBuf *out)
{
    size_t start = out->len;
    switch (t) {
    case AU_INPUT_URL:
    case AU_INPUT_EMAIL: {
        size_t a = 0, b = len;
        while (a < b && au_is_space((unsigned char)v[a]))
            a++;
        while (b > a && au_is_space((unsigned char)v[b - 1]))
            b--;
        for (size_t i = a; i < b; i++)
            if (v[i] != '\r' && v[i] != '\n')
                au_buf_putc(out, v[i]);
        break;
    }
    case AU_INPUT_NUMBER:
        if (valid_float(v, len))
            au_buf_append(out, v, len);
        break;
    case AU_INPUT_RANGE: {
        double mn = num_attr(el, "min", 0), mx = num_attr(el, "max", 100);
        if (mx < mn)
            mx = mn;
        double x = mn + (mx - mn) / 2;
        /* strtod on a copy: v is not NUL-terminated */
        if (valid_float(v, len)) {
            char *tmp = au_strndup(v, len);
            x = strtod(tmp, NULL);
            au_free(tmp);
        }
        if (x < mn)
            x = mn;
        if (x > mx)
            x = mx;
        double step = num_attr(el, "step", 1);
        if (step > 0) {
            double k = floor((x - mn) / step + 0.5);
            x = mn + k * step;
            if (x > mx)
                x -= step;
        }
        put_number(out, x);
        break;
    }
    case AU_INPUT_COLOR: {
        bool ok = len == 7 && v[0] == '#';
        for (size_t i = 1; ok && i < 7; i++)
            ok = au_is_hex((unsigned char)v[i]);
        if (ok) {
            for (size_t i = 0; i < 7; i++)
                au_buf_putc(out, (char)au_lower((unsigned char)v[i]));
        } else {
            au_buf_puts(out, "#000000");
        }
        break;
    }
    case AU_INPUT_HIDDEN:
    case AU_INPUT_CHECKBOX:
    case AU_INPUT_RADIO:
    case AU_INPUT_SUBMIT:
    case AU_INPUT_IMAGE:
    case AU_INPUT_RESET:
    case AU_INPUT_BUTTON:
    case AU_INPUT_FILE:
        au_buf_append(out, v, len);
        break;
    default:
        for (size_t i = 0; i < len; i++)
            if (v[i] != '\r' && v[i] != '\n')
                au_buf_putc(out, v[i]);
        break;
    }
    (void)start;
}

void au_ctl_default_value(const AuElement *el, AuBuf *out)
{
    if (is_html(el, AU_ATOM_textarea)) {
        for (AuNode *c = el->node.first_child; c; c = c->next) {
            if (c->type != AU_NODE_TEXT)
                continue;
            AuText *t = au_as_text(c);
            for (size_t i = 0; i < t->len; i++) {
                char ch = t->data[i];
                if (ch == '\r') {
                    au_buf_putc(out, '\n');
                    if (i + 1 < t->len && t->data[i + 1] == '\n')
                        i++;
                } else {
                    au_buf_putc(out, ch);
                }
            }
        }
        return;
    }
    const char *v = get_attr(el, "value");
    if (!is_html(el, AU_ATOM_input)) {
        if (v)
            au_buf_puts(out, v);
        return;
    }
    AuInputType t = au_input_type(el);
    if (!v)
        v = (t == AU_INPUT_CHECKBOX || t == AU_INPUT_RADIO) ? "on" : "";
    au_ctl_sanitize((AuElement *)el, t, v, strlen(v), out);
}

static void clamp_sel(AuControl *c)
{
    if (c->anchor > c->value.len)
        c->anchor = (uint32_t)c->value.len;
    if (c->caret > c->value.len)
        c->caret = (uint32_t)c->value.len;
    /* keep offsets on character boundaries */
    while (c->anchor > 0 && c->anchor < c->value.len && ((unsigned char)c->value.data[c->anchor] & 0xC0) == 0x80)
        c->anchor--;
    while (c->caret > 0 && c->caret < c->value.len && ((unsigned char)c->value.data[c->caret] & 0xC0) == 0x80)
        c->caret--;
}

const char *au_ctl_value(AuElement *el, size_t *len)
{
    AuControl *c = au_ctl(el);
    if (!c->dirty) {
        au_buf_clear(&c->value);
        au_ctl_default_value(el, &c->value);
        clamp_sel(c);
    }
    au_buf_cstr(&c->value);
    if (len)
        *len = c->value.len;
    return c->value.data;
}

void au_ctl_set_value(AuElement *el, const char *v, size_t len)
{
    AuControl *c = au_ctl(el);
    AuBuf tmp;
    au_buf_init(&tmp);
    if (is_html(el, AU_ATOM_input)) {
        au_ctl_sanitize(el, au_input_type(el), v, len, &tmp);
    } else {
        /* textarea: line breaks normalized to LF */
        for (size_t i = 0; i < len; i++) {
            if (v[i] == '\r') {
                au_buf_putc(&tmp, '\n');
                if (i + 1 < len && v[i + 1] == '\n')
                    i++;
            } else {
                au_buf_putc(&tmp, v[i]);
            }
        }
    }
    bool same = c->dirty && tmp.len == c->value.len && (!tmp.len || !memcmp(tmp.data, c->value.data, tmp.len));
    au_buf_clear(&c->value);
    au_buf_append(&c->value, tmp.data ? tmp.data : "", tmp.len);
    au_buf_free(&tmp);
    c->dirty = true;
    if (!same) {
        /* HTML: setting the value moves the caret to the end. */
        c->anchor = c->caret = (uint32_t)c->value.len;
        c->has_undo = false;
    }
    clamp_sel(c);
}

bool au_ctl_checked(const AuElement *el)
{
    if (el->state & AU_STATE_CHECKED_SET)
        return (el->state & AU_STATE_CHECKED) != 0;
    if (is_html(el, AU_ATOM_option))
        return au_element_get_attr(el, AU_ATOM_selected) != NULL;
    return get_attr(el, "checked") != NULL;
}

void au_ctl_set_checked(AuElement *el, bool on)
{
    au_element_set_state(el, AU_STATE_CHECKED_SET, true);
    au_element_set_state(el, AU_STATE_CHECKED, on);
}

void au_ctl_set_files(AuElement *el, AuControlFile *files, uint32_t n)
{
    AuControl *c = au_ctl(el);
    free_files(c);
    c->files = files;
    c->nfiles = n;
}

void au_ctl_reset(AuElement *el)
{
    AuControl *c = au_ctl_peek(el);
    if (c) {
        c->dirty = false;
        c->user_interacted = false;
        c->has_undo = false;
        c->scroll_x = c->scroll_y = 0;
        free_files(c);
        size_t n;
        au_ctl_value(el, &n);
        c->anchor = c->caret = (uint32_t)n;
    }
    if (el->state & AU_STATE_CHECKED_SET) {
        au_element_set_state(el, AU_STATE_CHECKED_SET, false);
        au_element_set_state(el, AU_STATE_CHECKED, false);
    }
    au_element_set_state(el, AU_STATE_INDETERMINATE, false);
}
