/* Forms in the document view: keyboard focus and tab order, mouse and key
 * input to form controls, activation (checkboxes, radio buttons, buttons,
 * labels, summary) when script is off, select pop-ups and list boxes,
 * file choosing, edit commands, constraint validation and submission.
 *
 * The value and selection of a text field live in its AuControl
 * (src/dom/control.h) and change through editing.c; layout reads them
 * when it paints (src/layout/controls.c), so typing repaints only the
 * field. With script on, the bindings run click activation (and fire
 * their submit events) and call back here to submit; key, input, change,
 * focus and submit events from user input are fired from here through
 * script.c. Without script, the same actions happen here, without
 * events. */
#include "docview_int.h"
#include "editing.h"
#include "formdata.h"

#include "../url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLINK_MS 530

static const char *get_attr(const AuElement *el, const char *name)
{
    AuAtom a = au_atom_lookup(name, strlen(name));
    return a ? au_element_get_attr(el, a) : NULL;
}

static bool is_html(const AuElement *el, AuAtom tag)
{
    return el && au_is_html_element(&el->node, tag);
}

static AuElement *parent_el(const AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_ELEMENT ? au_as_element(p) : NULL;
}

static void set_ref(AuElement **slot, AuElement *el)
{
    if (el)
        au_node_ref(&el->node);
    if (*slot)
        au_node_unref(&(*slot)->node);
    *slot = el;
}

static bool is_fixed(const AuElement *el)
{
    const AuStyle *s = el ? (const AuStyle *)el->style : NULL;
    return s && s->box->position == AU_POSITION_FIXED;
}

/* ---- Repainting ---- */

void au_dv_invalidate_element(AuDocView *dv, AuElement *el)
{
    if (!dv->cb.invalidate || !el)
        return;
    AuRect r;
    if (!au_layout_element_rect(dv->layout, el, &r))
        return;
    /* Fixed boxes are placed by the viewport scroll: cover both. */
    int pad = 4;
    AuIRect ir = {(int)r.x - pad, (int)r.y - pad, (int)(r.x + r.w) + pad + 1, (int)(r.y + r.h) + pad + 1};
    dv->cb.invalidate(dv->cb.ctx, dv, ir);
    if (is_fixed(el)) {
        AuIRect f = {ir.x0 + dv->scroll_x, ir.y0 + dv->scroll_y, ir.x1 + dv->scroll_x, ir.y1 + dv->scroll_y};
        dv->cb.invalidate(dv->cb.ctx, dv, f);
    }
}

/* A state bit of el changed (checked, focus): restyle and repaint. */
static void state_changed(AuDocView *dv, AuElement *el)
{
    if (dv->resolver)
        au_css_invalidate(dv->resolver, el, AU_CSS_CHANGE_STATE, 0);
    dv->restyle_needed = true;
    au_dv_invalidate_element(dv, el);
}

void au_forms_changed(AuDocView *dv, AuElement *el)
{
    au_dv_invalidate_element(dv, el);
}

/* The control's box shows text built with the box tree (a select's label,
 * a list box's rows): build it again. */
static void rebuild(AuDocView *dv, AuElement *el)
{
    dv->tree_dirty = true;
    au_dv_invalidate_element(dv, el);
}

static void show_caret(AuDocView *dv)
{
    dv->caret_on = true;
    au_layout_set_focus(dv->layout, dv->focus, true);
}

/* ---- Script events ---- */

static bool fire(AuDocView *dv, AuElement *el, const char *type, bool bubbles, bool cancelable)
{
    if (!dv->script || !el)
        return true;
    return au_docscript_fire(dv->script, el, type, bubbles, cancelable);
}

static bool alive(AuDocView *dv, AuElement *el)
{
    return dv->doc && el && el->node.doc == dv->doc && au_node_is_connected(&el->node);
}

/* input then change, after a user change of a checkbox, radio button,
 * select or file input. */
static void input_change(AuDocView *dv, AuElement *el)
{
    if (!dv->script)
        return;
    au_node_ref(&el->node);
    fire(dv, el, "input", true, false);
    if (alive(dv, el))
        fire(dv, el, "change", true, false);
    au_node_unref(&el->node);
}

/* Commits a text field: change when the value differs from when it was
 * focused (or last committed). */
static void commit_change(AuDocView *dv, AuElement *el)
{
    AuControl *c = au_ctl_peek(el);
    if (!c || !c->focus_value)
        return;
    size_t n;
    const char *v = au_ctl_value(el, &n);
    bool changed = n != c->focus_len || memcmp(v, c->focus_value, n) != 0;
    au_free(c->focus_value);
    c->focus_value = au_strndup(v, n);
    c->focus_len = n;
    if (changed)
        fire(dv, el, "change", true, false);
}

/* ---- Focus ---- */

static bool rendered(const AuElement *el)
{
    if (!el->box)
        return false;
    const AuStyle *s = el->style;
    return !s || s->inh->visibility == AU_VISIBILITY_VISIBLE;
}

static int tab_index(const AuElement *el, bool *has)
{
    const char *t = get_attr(el, "tabindex");
    *has = false;
    if (!t)
        return 0;
    while (au_is_space((unsigned char)*t))
        t++;
    char *end;
    long v = strtol(t, &end, 10);
    if (end == t)
        return 0;
    *has = true;
    return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)v;
}

/* Focusable by click or script (HTML "focusable area"). */
static bool focusable(const AuElement *el)
{
    if (!el || el->ns != AU_NS_HTML) {
        bool has;
        return el && el->ns == AU_NS_SVG && (tab_index(el, &has), has);
    }
    bool has;
    tab_index(el, &has);
    AuAtom t = el->tag;
    bool natural = false;
    if (t == AU_ATOM_a || t == AU_ATOM_area)
        natural = au_element_get_attr(el, au_atom_lookup("href", 4)) != NULL;
    else if (t == AU_ATOM_input)
        natural = au_input_type(el) != AU_INPUT_HIDDEN;
    else if (t == AU_ATOM_button || t == AU_ATOM_select || t == AU_ATOM_textarea)
        natural = true;
    else if (t == AU_ATOM_summary) {
        AuElement *d = parent_el(el);
        if (d && is_html(d, AU_ATOM_details)) {
            AuNode *first = NULL;
            for (AuNode *c = d->node.first_child; c && !first; c = c->next)
                if (au_is_html_element(c, AU_ATOM_summary))
                    first = c;
            natural = first == &el->node;
        }
    } else if (get_attr(el, "contenteditable"))
        natural = true;
    if (!natural && !has)
        return false;
    if (natural && au_form_is_listed(el) && au_form_disabled(el))
        return false;
    return rendered(el);
}

static bool tabbable(const AuElement *el)
{
    bool has;
    int ti = tab_index(el, &has);
    return focusable(el) && !(has && ti < 0);
}

static void fill_focus_state(AuDocView *dv, AuElement *el, bool on, bool visible)
{
    au_element_set_state(el, AU_STATE_FOCUS, on);
    au_element_set_state(el, AU_STATE_FOCUS_VISIBLE, on && visible);
    /* :focus-within on the ancestors */
    for (AuElement *p = parent_el(el); p; p = parent_el(p))
        if (dv->resolver)
            au_css_invalidate(dv->resolver, p, AU_CSS_CHANGE_STATE, 0);
    state_changed(dv, el);
}

/* Moves the focus to el (NULL: none). keyboard: Tab or a key moved it
 * (focus-visible, and a text field's whole value is selected). */
static void focus_el(AuDocView *dv, AuElement *el, bool keyboard, bool from_script)
{
    if (el == dv->focus) {
        if (el && keyboard && !dv->focus_visible) {
            dv->focus_visible = true;
            fill_focus_state(dv, el, true, true);
        }
        return;
    }
    AuElement *old = dv->focus;
    if (old) {
        au_node_ref(&old->node);
        if (au_ctl_is_text_field(old) && !from_script)
            commit_change(dv, old);
        if (dv->focus != old) { /* a change listener moved the focus */
            au_node_unref(&old->node);
            return;
        }
        fill_focus_state(dv, old, false, false);
        AuControl *c = au_ctl_peek(old);
        if (c) {
            au_free(c->focus_value);
            c->focus_value = NULL;
        }
        dv->focus = NULL;
    }
    if (el) {
        if (dv->focus_frame)
            au_frames_blur(dv); /* the keyboard leaves the nested document */
        set_ref(&dv->focus, el);
        bool text = au_ctl_is_text_field(el);
        dv->focus_visible = keyboard || text;
        fill_focus_state(dv, el, true, dv->focus_visible);
        if (text) {
            size_t n;
            const char *v = au_ctl_value(el, &n);
            AuControl *c = au_ctl(el);
            au_free(c->focus_value);
            c->focus_value = au_strndup(v, n);
            c->focus_len = n;
            if (keyboard)
                au_edit_select_all(c);
            dv->goal_x = -1;
        }
    }
    dv->caret_on = true;
    au_layout_set_focus(dv->layout, dv->focus, true);
    if (dv->script && !from_script) {
        AuElement *now = dv->focus;
        au_docscript_focus(dv->script, now);
    }
    if (old)
        au_node_unref(&old->node);
}

void au_forms_script_focus(AuDocView *dv, AuElement *el)
{
    if (el && (!alive(dv, el)))
        return;
    focus_el(dv, el, false, true);
}

/* At the end of parsing: the first focusable element with autofocus gets
 * the focus, unless something already has it. */
void au_forms_autofocus(AuDocView *dv)
{
    if (!dv->doc || dv->focus)
        return;
    AuAtom af = au_atom_lookup("autofocus", 9);
    AuElement *cand = NULL;
    for (AuNode *n = au_node_next(&dv->doc->node, &dv->doc->node); n; n = au_node_next(n, &dv->doc->node))
        if (n->type == AU_NODE_ELEMENT && au_element_get_attr(au_as_element(n), af)) {
            cand = au_as_element(n);
            break;
        }
    if (!cand)
        return;
    au_docview_update(dv);
    if (alive(dv, cand) && focusable(cand) && !dv->focus)
        focus_el(dv, cand, false, false);
}

void au_forms_clear(AuDocView *dv)
{
    set_ref(&dv->focus, NULL);
    set_ref(&dv->drag, NULL);
    set_ref(&dv->pressed, NULL);
    au_layout_set_focus(dv->layout, NULL, false);
}

AuElement *au_docview_focus(const AuDocView *dv)
{
    return dv->focus;
}

void au_docview_set_focus(AuDocView *dv, AuElement *el)
{
    if (el && (!alive(dv, el) || !focusable(el)))
        return;
    focus_el(dv, el, false, false);
}

bool au_docview_editing(const AuDocView *dv)
{
    return dv->focus && au_ctl_is_text_field(dv->focus) && alive((AuDocView *)dv, dv->focus);
}

/* Scrolls the viewport so el is visible. */
static void reveal_element(AuDocView *dv, AuElement *el)
{
    AuRect r;
    if (!dv->cb.scroll_to || !au_layout_element_rect(dv->layout, el, &r))
        return;
    if (is_fixed(el))
        return;
    int x = dv->scroll_x, y = dv->scroll_y;
    if (r.y < y || r.y + r.h > y + dv->vh)
        y = (int)r.y - dv->vh / 3;
    if (r.x < x || r.x + r.w > x + dv->vw)
        x = (int)r.x - 20;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x != dv->scroll_x || y != dv->scroll_y)
        dv->cb.scroll_to(dv->cb.ctx, dv, x, y);
}

/* Tab order: positive tabindex ascending (tree order among equals), then
 * the rest in tree order. */
static AuElement *next_tab(AuDocView *dv, AuElement *from, bool back)
{
    size_t n = 0, cap = 64;
    AuElement **all = au_alloc(cap * sizeof *all);
    int *key = au_alloc(cap * sizeof *key);
    AuNode *root = &dv->doc->node;
    for (AuNode *c = root; c; c = au_node_next(c, root)) {
        if (c->type != AU_NODE_ELEMENT || !tabbable(au_as_element(c)))
            continue;
        if (n == cap) {
            cap *= 2;
            all = au_realloc(all, cap * sizeof *all);
            key = au_realloc(key, cap * sizeof *key);
        }
        bool has;
        int ti = tab_index(au_as_element(c), &has);
        all[n] = au_as_element(c);
        key[n] = ti > 0 ? ti : 0x7FFFFFFF;
        n++;
    }
    /* stable insertion sort by key */
    for (size_t i = 1; i < n; i++) {
        AuElement *e = all[i];
        int k = key[i];
        size_t j = i;
        while (j > 0 && key[j - 1] > k) {
            all[j] = all[j - 1];
            key[j] = key[j - 1];
            j--;
        }
        all[j] = e;
        key[j] = k;
    }
    AuElement *r = NULL;
    if (n) {
        size_t at = n;
        for (size_t i = 0; i < n; i++)
            if (all[i] == from)
                at = i;
        if (at == n)
            r = back ? all[n - 1] : all[0];
        else
            r = all[back ? (at + n - 1) % n : (at + 1) % n];
    }
    au_free(all);
    au_free(key);
    return r;
}

/* ---- Caret blink ---- */

uint32_t au_docview_blink(AuDocView *dv, uint32_t now_ms)
{
    if (!au_docview_editing(dv)) {
        dv->blink_ms = now_ms;
        return UINT32_MAX;
    }
    uint32_t dt = now_ms - dv->blink_ms;
    if (dt < BLINK_MS)
        return BLINK_MS - dt;
    dv->blink_ms = now_ms;
    AuControl *c = au_ctl(dv->focus);
    if (c->anchor != c->caret)
        return BLINK_MS; /* no caret over a selection */
    dv->caret_on = !dv->caret_on;
    au_layout_set_focus(dv->layout, dv->focus, dv->caret_on);
    AuRect r;
    if (dv->cb.invalidate && au_layout_field_caret_rect(dv->layout, dv->focus, c->caret, &r)) {
        AuIRect ir = {(int)r.x - 1, (int)r.y - 1, (int)(r.x + r.w) + 2, (int)(r.y + r.h) + 2};
        dv->cb.invalidate(dv->cb.ctx, dv, ir);
        if (is_fixed(dv->focus)) {
            AuIRect f = {ir.x0 + dv->scroll_x, ir.y0 + dv->scroll_y, ir.x1 + dv->scroll_x, ir.y1 + dv->scroll_y};
            dv->cb.invalidate(dv->cb.ctx, dv, f);
        }
    }
    return BLINK_MS;
}

/* ---- Submission ---- */

static int pattern_test(void *ctx, const char *pattern, const char *value)
{
    AuDocView *dv = ctx;
    return dv->script ? au_docscript_pattern(dv->script, pattern, value) : -1;
}

/* Interactive validation: true when every control is valid. Otherwise
 * invalid fires at each invalid control, and the first one (whose event
 * was not cancelled) gets the focus and its message is shown. */
static bool validate_form(AuDocView *dv, AuElement *form)
{
    size_t n;
    AuElement **els = au_form_elements(form, &n);
    for (size_t i = 0; i < n; i++)
        au_node_ref(&els[i]->node);
    AuElement *first = NULL;
    unsigned first_bits = 0;
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        AuElement *e = els[i];
        if (!alive(dv, e))
            continue;
        unsigned bits = au_form_validity(e, pattern_test, dv);
        if (!bits)
            continue;
        ok = false;
        if (fire(dv, e, "invalid", false, true) && !first && alive(dv, e)) {
            first = e;
            first_bits = bits;
        }
    }
    if (first && dv->doc) {
        reveal_element(dv, first);
        if (focusable(first))
            focus_el(dv, first, true, false);
        if (dv->cb.message)
            dv->cb.message(dv->cb.ctx, dv, au_form_validity_message(first, first_bits));
    }
    for (size_t i = 0; i < n; i++)
        au_node_unref(&els[i]->node);
    au_free(els);
    return ok;
}

enum { SUB_NO_VALIDATE = 1, SUB_NO_EVENTS = 2 };

/* The submitter's override of a form attribute (formaction...), else the
 * form's. */
static const char *pick(AuElement *form, AuElement *submitter, const char *name)
{
    char fname[32];
    snprintf(fname, sizeof fname, "form%s", name);
    const char *v = submitter ? get_attr(submitter, fname) : NULL;
    return v ? v : get_attr(form, name);
}

static bool starts_ci(const char *s, const char *p)
{
    size_t n = strlen(p);
    return strlen(s) >= n && au_ieq(s, n, p, n);
}

static void form_submit(AuDocView *dv, AuElement *form, AuElement *submitter, int flags, int ix, int iy)
{
    if (dv->submitting || !alive(dv, form))
        return;
    if (dv->sandbox & AU_SANDBOX_FORMS)
        return; /* a sandboxed frame without allow-forms */
    au_node_ref(&form->node);
    if (submitter)
        au_node_ref(&submitter->node);
    dv->submitting = true;
    bool novalidate = get_attr(form, "novalidate") || (submitter && get_attr(submitter, "formnovalidate"));
    if (!(flags & SUB_NO_VALIDATE) && !novalidate && !validate_form(dv, form))
        goto out;
    if (!(flags & SUB_NO_EVENTS) && dv->script) {
        if (!au_docscript_submit_event(dv->script, form, submitter) || !alive(dv, form))
            goto out;
    }
    {
        const char *method = pick(form, submitter, "method");
        bool post = method && au_ieq(method, strlen(method), "post", 4);
        if (method && au_ieq(method, strlen(method), "dialog", 6)) {
            for (AuElement *p = parent_el(form); p; p = parent_el(p))
                if (is_html(p, AU_ATOM_dialog)) {
                    au_element_remove_attr(p, au_atom("open", 4));
                    dv->tree_dirty = true;
                    break;
                }
            goto out;
        }
        const char *action = pick(form, submitter, "action");
        char *url = au_url_resolve_str(dv->base, action && *action ? action : dv->url);
        if (!url)
            goto out;
        if (starts_ci(url, "javascript:")) {
            if (dv->script)
                au_docscript_javascript_url(dv->script, url);
            au_free(url);
            goto out;
        }
        const char *enctype = pick(form, submitter, "enctype");
        int enc = 0; /* 0 urlencoded, 1 multipart, 2 text/plain */
        if (enctype && au_ieq(enctype, strlen(enctype), "multipart/form-data", 19))
            enc = 1;
        else if (enctype && au_ieq(enctype, strlen(enctype), "text/plain", 10))
            enc = 2;
        const char *target = pick(form, submitter, "target");
        if (!target) {
            /* <base target> */
            for (AuNode *c = &dv->doc->node; c && !target; c = au_node_next(c, &dv->doc->node))
                if (au_is_html_element(c, AU_ATOM_base))
                    target = get_attr(au_as_element(c), "target");
        }
        bool new_tab = target && *target && !au_ieq(target, strlen(target), "_self", 5) &&
                       !au_ieq(target, strlen(target), "_top", 4) && !au_ieq(target, strlen(target), "_parent", 7);
        AuFormEntries l;
        au_form_entries_init(&l);
        au_form_entries_build(form, submitter, ix, iy, &l);
        if (!alive(dv, form)) {
            au_form_entries_free(&l);
            au_free(url);
            goto out;
        }
        /* Insecure submission: an https page sending to http. */
        if (starts_ci(dv->url, "https:") && starts_ci(url, "http:") && dv->cb.confirm &&
            !dv->cb.confirm(dv->cb.ctx, dv,
                            "The information you have entered on this page will be sent over an insecure "
                            "connection and could be read by a third party. Send it anyway?")) {
            au_form_entries_free(&l);
            au_free(url);
            goto out;
        }
        AuBuf body, target_url;
        au_buf_init(&body);
        au_buf_init(&target_url);
        char boundary[64];
        AuFormSubmission s;
        memset(&s, 0, sizeof s);
        s.new_tab = new_tab;
        if (!post || starts_ci(url, "mailto:")) {
            /* GET: the query is replaced, the fragment kept. */
            char *hash = strchr(url, '#');
            size_t base_len = hash ? (size_t)(hash - url) : strlen(url);
            char *q = memchr(url, '?', base_len);
            au_buf_append(&target_url, url, q ? (size_t)(q - url) : base_len);
            au_buf_putc(&target_url, '?');
            if (enc == 2 && starts_ci(url, "mailto:")) {
                AuBuf t;
                au_buf_init(&t);
                au_form_text_plain(&l, &t);
                au_buf_puts(&target_url, "body=");
                au_pct_encode(&target_url, t.data ? t.data : "", t.len, AU_PCT_COMPONENT);
                au_buf_free(&t);
            } else {
                au_form_urlencode(&l, &target_url);
            }
            if (hash)
                au_buf_puts(&target_url, hash);
            s.method = "GET";
        } else {
            au_buf_puts(&target_url, url);
            s.method = "POST";
            if (enc == 1) {
                uint32_t r = (uint32_t)(uintptr_t)form ^ (uint32_t)(au_dv_now(dv) * 1000.0) ^ (uint32_t)l.n * 2654435761u;
                uint32_t r2 = r * 2246822519u + 0x9E3779B9u;
                snprintf(boundary, sizeof boundary, "----AuraFormBoundary%08lx%08lx", (unsigned long)r,
                         (unsigned long)r2);
                au_form_multipart(&l, boundary, &body);
                char *ty = au_alloc(strlen(boundary) + 40);
                sprintf(ty, "multipart/form-data; boundary=%s", boundary);
                s.body_type = ty;
            } else if (enc == 2) {
                au_form_text_plain(&l, &body);
                s.body_type = "text/plain;charset=UTF-8";
            } else {
                au_form_urlencode(&l, &body);
                s.body_type = "application/x-www-form-urlencoded";
            }
            s.body = body.data ? body.data : "";
            s.body_len = body.len;
        }
        s.url = au_buf_cstr(&target_url);
        s.target = target;
        /* Into a frame, the tab or a new tab, as the target says. */
        AuDocView *to = NULL, *top = au_dv_top(dv);
        int where = au_frames_route(dv, target, false, true, &to);
        s.new_tab = where == 3;
        if (where == 1)
            au_frames_navigate(to, s.url, &s);
        else if (where >= 2 && top->cb.submit)
            top->cb.submit(top->cb.ctx, top, &s);
        else if (where >= 2 && !post && top->cb.open_url)
            top->cb.open_url(top->cb.ctx, top, s.url, s.new_tab);
        if (enc == 1 && post)
            au_free((char *)s.body_type);
        au_buf_free(&body);
        au_buf_free(&target_url);
        au_form_entries_free(&l);
        au_free(url);
    }
out:
    dv->submitting = false;
    if (submitter)
        au_node_unref(&submitter->node);
    au_node_unref(&form->node);
}

void au_forms_script_submit(AuDocView *dv, AuElement *form, AuElement *submitter)
{
    form_submit(dv, form, submitter, SUB_NO_VALIDATE | SUB_NO_EVENTS, 0, 0);
}

void au_docview_submit(AuDocView *dv, AuElement *form, AuElement *submitter)
{
    form_submit(dv, form, submitter, 0, 0, 0);
}

static void form_reset(AuDocView *dv, AuElement *form)
{
    if (!fire(dv, form, "reset", true, true) || !alive(dv, form))
        return;
    size_t n;
    AuElement **els = au_form_elements(form, &n);
    for (size_t i = 0; i < n; i++) {
        au_ctl_reset(els[i]);
        if (is_html(els[i], AU_ATOM_select)) {
            size_t no;
            AuElement **o = au_select_options(els[i], false, &no);
            for (size_t k = 0; k < no; k++)
                au_ctl_reset(o[k]);
            au_free(o);
        }
        state_changed(dv, els[i]);
    }
    au_free(els);
    dv->tree_dirty = true;
}

/* Fields that block implicit submission: text-like inputs. */
static void implicit_submit(AuDocView *dv, AuElement *field)
{
    AuElement *form = au_form_owner(field);
    if (!form)
        return;
    AuElement *btn = au_form_default_button(form);
    if (btn) {
        if (au_form_disabled(btn))
            return;
        if (au_docscript_running(dv->script)) {
            AuEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.clicks = 1;
            au_docscript_mouse(dv->script, &ev, btn, "click", 0, 0);
        } else {
            au_forms_activate(dv, btn, 0, 0);
        }
        return;
    }
    size_t n, blocking = 0;
    AuElement **els = au_form_elements(form, &n);
    for (size_t i = 0; i < n; i++)
        if (au_ctl_is_single_line(els[i]))
            blocking++;
    au_free(els);
    if (blocking <= 1)
        form_submit(dv, form, NULL, 0, 0, 0);
}

/* ---- Activation ---- */

static AuElement *label_control(AuElement *label)
{
    const char *f = get_attr(label, "for");
    if (f) {
        AuAtom a = au_atom_lookup(f, strlen(f));
        if (!a)
            return NULL;
        AuNode *root = au_node_root(&label->node);
        for (AuNode *c = root; c; c = au_node_next(c, root))
            if (c->type == AU_NODE_ELEMENT && au_as_element(c)->id == a) {
                AuElement *e = au_as_element(c);
                return e->ns == AU_NS_HTML && (e->tag == AU_ATOM_input || e->tag == AU_ATOM_select ||
                                                e->tag == AU_ATOM_textarea || e->tag == AU_ATOM_button ||
                                                e->tag == AU_ATOM_meter || e->tag == AU_ATOM_output ||
                                                e->tag == AU_ATOM_progress)
                           ? e
                           : NULL;
            }
        return NULL;
    }
    for (AuNode *c = au_node_next(&label->node, &label->node); c; c = au_node_next(c, &label->node)) {
        if (c->type != AU_NODE_ELEMENT)
            continue;
        AuElement *e = au_as_element(c);
        if (e->ns == AU_NS_HTML &&
            (e->tag == AU_ATOM_select || e->tag == AU_ATOM_textarea || e->tag == AU_ATOM_button ||
             (e->tag == AU_ATOM_input && au_input_type(e) != AU_INPUT_HIDDEN)))
            return e;
    }
    return NULL;
}

static bool node_contains(const AuNode *a, const AuNode *b)
{
    for (; b; b = b->parent)
        if (a == b)
            return true;
    return false;
}

static void choose_file(AuDocView *dv, AuElement *el)
{
    if (!dv->cb.choose_file)
        return;
    AuControlFile *files = NULL;
    uint32_t n = 0;
    au_node_ref(&el->node);
    if (dv->cb.choose_file(dv->cb.ctx, dv, get_attr(el, "multiple") != NULL, &files, &n) && alive(dv, el)) {
        au_ctl_set_files(el, files, n);
        au_ctl(el)->user_interacted = true;
        rebuild(dv, el);
        input_change(dv, el);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            au_free(files[i].name);
            au_free(files[i].type);
            au_free(files[i].data);
        }
        au_free(files);
    }
    au_node_unref(&el->node);
}

static void toggle_details(AuDocView *dv, AuElement *summary)
{
    AuElement *d = parent_el(summary);
    if (!d || !is_html(d, AU_ATOM_details))
        return;
    AuAtom open = au_atom("open", 4);
    if (au_element_get_attr(d, open))
        au_element_remove_attr(d, open);
    else
        au_element_set_attr(d, open, "", 0);
    dv->tree_dirty = true;
    dv->restyle_needed = true;
    if (dv->resolver)
        au_css_invalidate(dv->resolver, d, AU_CSS_CHANGE_ATTR, open);
    if (dv->cb.invalidate) {
        float w, h;
        au_layout_document_size(dv->layout, &w, &h);
        dv->cb.invalidate(dv->cb.ctx, dv, (AuIRect){0, 0, (int)w + 1, (int)h + 1});
    }
    fire(dv, d, "toggle", false, false);
}

static void activate_depth(AuDocView *dv, AuElement *target, int x, int y, int depth)
{
    AuElement *act = NULL;
    for (AuElement *e = target; e; e = parent_el(e)) {
        if (e->ns != AU_NS_HTML)
            continue;
        AuAtom t = e->tag;
        if (t == AU_ATOM_a || t == AU_ATOM_area) {
            if (au_element_get_attr(e, au_atom_lookup("href", 4)))
                return; /* links are the view's */
            continue;
        }
        if ((t == AU_ATOM_input && au_input_type(e) != AU_INPUT_HIDDEN) || t == AU_ATOM_button ||
            t == AU_ATOM_label || t == AU_ATOM_summary) {
            act = e;
            break;
        }
    }
    if (!act)
        return;
    if (is_html(act, AU_ATOM_summary)) {
        toggle_details(dv, act);
        return;
    }
    if (is_html(act, AU_ATOM_label)) {
        AuElement *c = label_control(act);
        if (c && depth < 4 && !node_contains(&c->node, &target->node)) {
            if (au_ctl_is_text_field(c) || is_html(c, AU_ATOM_select))
                focus_el(dv, c, false, false);
            else
                activate_depth(dv, c, 0, 0, depth + 1);
        }
        return;
    }
    if (au_form_disabled(act))
        return;
    if (is_html(act, AU_ATOM_input)) {
        AuInputType t = au_input_type(act);
        if (t == AU_INPUT_CHECKBOX) {
            au_ctl_set_checked(act, !au_ctl_checked(act));
            au_element_set_state(act, AU_STATE_INDETERMINATE, false);
            state_changed(dv, act);
            return;
        }
        if (t == AU_INPUT_RADIO) {
            if (au_ctl_checked(act))
                return;
            size_t n;
            AuElement **g = au_form_radio_group(act, &n);
            for (size_t i = 0; i < n; i++)
                if (g[i] != act && au_ctl_checked(g[i])) {
                    au_ctl_set_checked(g[i], false);
                    state_changed(dv, g[i]);
                }
            au_free(g);
            au_ctl_set_checked(act, true);
            state_changed(dv, act);
            return;
        }
        if (t == AU_INPUT_FILE) {
            choose_file(dv, act);
            return;
        }
    }
    AuElement *form = au_form_owner(act);
    if (!form)
        return;
    if (au_form_is_submit_button(act)) {
        form_submit(dv, form, act, 0, x, y);
        return;
    }
    const char *ty = au_element_get_attr(act, AU_ATOM_type);
    bool reset = (is_html(act, AU_ATOM_input) && au_input_type(act) == AU_INPUT_RESET) ||
                 (is_html(act, AU_ATOM_button) && ty && au_ieq(ty, strlen(ty), "reset", 5));
    if (reset)
        form_reset(dv, form);
}

void au_forms_activate(AuDocView *dv, AuElement *target, int x, int y)
{
    if (target)
        activate_depth(dv, target, x, y, 0);
}

/* A click on el as the keyboard makes it (Space on a checkbox, Return on
 * a button): with script, a click event whose activation the bindings
 * run. */
static void synthetic_click(AuDocView *dv, AuElement *el)
{
    if (au_docscript_running(dv->script)) {
        AuEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.clicks = 1;
        au_docscript_mouse(dv->script, &ev, el, "click", 0, 0);
        /* the bindings toggled checkboxes and radio buttons */
        if (alive(dv, el))
            state_changed(dv, el);
    } else {
        au_forms_activate(dv, el, 0, 0);
    }
}

/* ---- Select ---- */

/* Applies a user choice of option index idx in a drop-down. */
static void select_choose(AuDocView *dv, AuElement *sel, int idx)
{
    if (idx < 0 || idx == au_select_index(sel))
        return;
    au_select_set_index(sel, idx);
    au_ctl(sel)->user_interacted = true;
    rebuild(dv, sel);
    input_change(dv, sel);
}

/* Without a menu, a click chooses the next enabled option (wrapping). */
static void select_cycle(AuDocView *dv, AuElement *sel)
{
    int cur = au_select_index(sel);
    size_t no;
    AuElement **o = au_select_options(sel, false, &no);
    for (size_t k = 1; k <= no; k++) {
        int i = (int)((cur + k) % no);
        if (!au_form_disabled(o[i])) {
            select_choose(dv, sel, i);
            break;
        }
    }
    au_free(o);
}

static void select_popup(AuDocView *dv, AuElement *sel)
{
    size_t n;
    AuElement **rows = au_select_options(sel, true, &n);
    int cur = au_select_index(sel);
    if (!n) {
        au_free(rows);
        return;
    }
    if (!dv->cb.popup) {
        au_free(rows);
        select_cycle(dv, sel);
        return;
    }
    AuPopupItem *items = au_calloc(n * sizeof *items);
    AuBuf labels;
    au_buf_init(&labels);
    size_t *offs = au_alloc(n * sizeof *offs);
    int *opt_index = au_alloc(n * sizeof *opt_index);
    int oi = 0, selected_row = -1;
    for (size_t i = 0; i < n; i++) {
        offs[i] = labels.len;
        AuElement *r = rows[i];
        if (is_html(r, AU_ATOM_optgroup)) {
            const char *lab = get_attr(r, "label");
            au_buf_puts(&labels, lab ? lab : "");
            items[i].group = true;
            items[i].disabled = true;
            opt_index[i] = -1;
        } else {
            au_option_label(r, &labels);
            items[i].disabled = au_form_disabled(r);
            items[i].indent = parent_el(r) != sel;
            if (oi == cur)
                selected_row = (int)i;
            opt_index[i] = oi++;
        }
        au_buf_putc(&labels, 0);
    }
    for (size_t i = 0; i < n; i++)
        items[i].label = labels.data + offs[i];
    AuRect r;
    AuIRect at = {0, 0, 0, 0};
    if (au_layout_element_rect(dv->layout, sel, &r))
        at = (AuIRect){(int)r.x, (int)r.y, (int)(r.x + r.w), (int)(r.y + r.h)};
    au_node_ref(&sel->node);
    int chosen = dv->cb.popup(dv->cb.ctx, dv, items, (int)n, selected_row, at);
    if (chosen >= 0 && chosen < (int)n && opt_index[chosen] >= 0 && !items[chosen].disabled && alive(dv, sel))
        select_choose(dv, sel, opt_index[chosen]);
    else if (chosen == -2 && alive(dv, sel))
        select_cycle(dv, sel); /* the embedder has no menu after all */
    au_node_unref(&sel->node);
    au_free(items);
    au_free(offs);
    au_free(opt_index);
    au_buf_free(&labels);
    au_free(rows);
}

/* Moves a drop-down's choice by dir (skipping disabled options). */
static void select_step(AuDocView *dv, AuElement *sel, int dir)
{
    size_t n;
    AuElement **o = au_select_options(sel, false, &n);
    int i = au_select_index(sel);
    for (int k = i + dir; k >= 0 && k < (int)n; k += dir)
        if (!au_form_disabled(o[k])) {
            select_choose(dv, sel, k);
            break;
        }
    au_free(o);
}

/* List box: row r (in rows with groups) clicked or reached by a key. */
static void listbox_pick(AuDocView *dv, AuElement *sel, int row, bool toggle, bool extend)
{
    size_t n;
    AuElement **rows = au_select_options(sel, true, &n);
    if (row < 0 || row >= (int)n || is_html(rows[row], AU_ATOM_optgroup) || au_form_disabled(rows[row])) {
        au_free(rows);
        return;
    }
    AuControl *c = au_ctl(sel);
    bool multi = au_element_get_attr(sel, AU_ATOM_multiple) != NULL;
    bool changed = false;
    if (multi && toggle) {
        au_ctl_set_checked(rows[row], !au_ctl_checked(rows[row]));
        changed = true;
    } else if (multi && extend && c->cursor >= 0) {
        int a = c->cursor < row ? c->cursor : row, b = c->cursor < row ? row : c->cursor;
        for (int i = 0; i < (int)n; i++) {
            if (is_html(rows[i], AU_ATOM_optgroup))
                continue;
            bool on = i >= a && i <= b && !au_form_disabled(rows[i]);
            if (on != au_ctl_checked(rows[i])) {
                au_ctl_set_checked(rows[i], on);
                changed = true;
            }
        }
    } else {
        for (int i = 0; i < (int)n; i++) {
            if (is_html(rows[i], AU_ATOM_optgroup))
                continue;
            bool on = i == row;
            if (on != au_ctl_checked(rows[i]) || !(rows[i]->state & AU_STATE_CHECKED_SET)) {
                changed |= on != au_ctl_checked(rows[i]);
                au_ctl_set_checked(rows[i], on);
            }
        }
    }
    if (!extend)
        c->cursor = row;
    /* keep the row in view */
    int vis = au_layout_listbox_visible(dv->layout, sel);
    if (row < (int)c->scroll_y)
        c->scroll_y = (float)row;
    else if (row >= (int)c->scroll_y + vis)
        c->scroll_y = (float)(row - vis + 1);
    au_free(rows);
    rebuild(dv, sel);
    if (changed) {
        c->user_interacted = true;
        input_change(dv, sel);
    }
}

/* ---- Range ---- */

static void range_set(AuDocView *dv, AuElement *el, double v, bool commit)
{
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.10g", v);
    size_t on;
    const char *old = au_ctl_value(el, &on);
    char *was = au_strndup(old, on);
    au_ctl_set_value(el, tmp, strlen(tmp));
    size_t nn;
    const char *now = au_ctl_value(el, &nn);
    bool changed = strcmp(was, now) != 0;
    au_free(was);
    au_ctl(el)->user_interacted = true;
    if (changed) {
        /* the slider is painted from the value attribute's box data */
        rebuild(dv, el);
        fire(dv, el, "input", true, false);
    }
    if (commit && changed && alive(dv, el))
        fire(dv, el, "change", true, false);
}

static double range_num(AuElement *el, const char *name, double dflt)
{
    const char *v = get_attr(el, name);
    char *end;
    double d = v ? strtod(v, &end) : 0;
    return v && end != v ? d : dflt;
}

/* ---- Text editing ---- */

/* What selectors can see of a text field's value: whether it is empty
 * (:placeholder-shown) and its validity bits (:valid, :invalid,
 * :in-range). An edit restyles only when this changes; typing one more
 * character into a field does not. */
static unsigned field_style_state(AuDocView *dv, AuElement *el)
{
    size_t n = 0;
    au_ctl_value(el, &n);
    return (n ? 0u : 1u) | (au_form_validity(el, pattern_test, dv) << 1);
}

/* After an edit: input event, scroll the caret into view, repaint the
 * field. before is field_style_state from before the edit. */
static void edited(AuDocView *dv, AuElement *el, const char *input_type, const char *data, unsigned before)
{
    AuControl *c = au_ctl(el);
    c->user_interacted = true;
    au_layout_field_reveal(dv->layout, el);
    show_caret(dv);
    if (dv->resolver && field_style_state(dv, el) != before)
        state_changed(dv, el); /* restyles, and repaints the field */
    else
        au_dv_invalidate_element(dv, el);
    if (dv->script)
        au_docscript_input(dv->script, el, "input", input_type, data);
}

static bool before_input(AuDocView *dv, AuElement *el, const char *input_type, const char *data)
{
    if (!dv->script)
        return true;
    bool ok = au_docscript_input(dv->script, el, "beforeinput", input_type, data);
    return ok && alive(dv, el) && dv->focus == el;
}

static bool readonly(const AuElement *el)
{
    return get_attr(el, "readonly") != NULL;
}

static long max_chars(const AuElement *el)
{
    const char *m = get_attr(el, "maxlength");
    if (!m || !au_is_digit((unsigned char)*m))
        return -1;
    return strtol(m, NULL, 10);
}

static bool insert_text(AuDocView *dv, AuElement *el, const char *s, size_t n, int kind, const char *input_type)
{
    if (readonly(el))
        return false;
    char *data = au_strndup(s, n);
    bool ok = false;
    if (before_input(dv, el, input_type, data)) {
        AuControl *c = au_ctl(el);
        unsigned before = field_style_state(dv, el);
        if (au_edit_insert(c, s, n, max_chars(el), au_ctl_is_single_line(el), kind)) {
            edited(dv, el, input_type, data, before);
            ok = true;
        }
    }
    au_free(data);
    return ok;
}

static bool delete_text(AuDocView *dv, AuElement *el, int dir, bool word, bool to_line)
{
    if (readonly(el))
        return false;
    AuControl *c = au_ctl(el);
    au_ctl_value(el, NULL);
    const char *it = dir < 0 ? (word ? "deleteWordBackward" : to_line ? "deleteSoftLineBackward" : "deleteContentBackward")
                             : (word ? "deleteWordForward" : to_line ? "deleteSoftLineForward" : "deleteContentForward");
    if (!before_input(dv, el, it, NULL))
        return false;
    c = au_ctl(el);
    unsigned before = field_style_state(dv, el);
    bool done;
    if (to_line && !au_edit_has_selection(c))
        done = au_edit_delete_to(c, au_layout_field_line_edge(dv->layout, el, c->caret, dir));
    else
        done = au_edit_delete(c, dir, word);
    if (done)
        edited(dv, el, it, NULL, before);
    return done;
}

static void moved(AuDocView *dv, AuElement *el)
{
    au_layout_field_reveal(dv->layout, el);
    show_caret(dv);
    au_dv_invalidate_element(dv, el);
}

/* Keys in a focused text field. */
static bool field_key(AuDocView *dv, AuElement *el, const AuEvent *ev)
{
    AuControl *c = au_ctl(el);
    au_ctl_value(el, NULL);
    bool shift = (ev->mods & AU_MOD_SHIFT) != 0, cmd = (ev->mods & AU_MOD_COMMAND) != 0,
         opt = (ev->mods & AU_MOD_OPTION) != 0;
    bool multi = !au_ctl_is_single_line(el);
    if (ev->key != AU_KEY_UP && ev->key != AU_KEY_DOWN)
        dv->goal_x = -1;
    switch (ev->key) {
    case AU_KEY_LEFT:
    case AU_KEY_RIGHT: {
        int dir = ev->key == AU_KEY_LEFT ? -1 : 1;
        if (cmd) {
            if (!shift && au_edit_has_selection(c)) {
                uint32_t a, b;
                au_edit_range(c, &a, &b);
                au_edit_move_to(c, dir < 0 ? a : b, false);
            }
            au_edit_move_to(c, au_layout_field_line_edge(dv->layout, el, c->caret, dir), shift);
        } else {
            au_edit_move(c, dir, opt, shift);
        }
        moved(dv, el);
        return true;
    }
    case AU_KEY_UP:
    case AU_KEY_DOWN: {
        int dir = ev->key == AU_KEY_UP ? -1 : 1;
        uint32_t to;
        if (cmd || !multi) {
            to = dir < 0 ? 0 : (uint32_t)c->value.len;
            dv->goal_x = -1;
        } else {
            if (!shift && au_edit_has_selection(c)) {
                uint32_t a, b;
                au_edit_range(c, &a, &b);
                au_edit_move_to(c, dir < 0 ? a : b, false);
            }
            to = au_layout_field_vmove(dv->layout, el, c->caret, dir, &dv->goal_x);
        }
        au_edit_move_to(c, to, shift);
        moved(dv, el);
        return true;
    }
    case AU_KEY_HOME:
    case AU_KEY_END: {
        int dir = ev->key == AU_KEY_HOME ? -1 : 1;
        uint32_t to = cmd || !multi ? (dir < 0 ? 0 : (uint32_t)c->value.len)
                                    : au_layout_field_line_edge(dv->layout, el, c->caret, dir);
        au_edit_move_to(c, to, shift);
        moved(dv, el);
        return true;
    }
    case AU_KEY_BACKSPACE:
        delete_text(dv, el, -1, opt, cmd);
        return true;
    case AU_KEY_DELETE:
        delete_text(dv, el, 1, opt, cmd);
        return true;
    case AU_KEY_RETURN:
        if (multi) {
            insert_text(dv, el, "\n", 1, AU_EDIT_OTHER, "insertLineBreak");
            return true;
        }
        au_node_ref(&el->node);
        commit_change(dv, el);
        if (alive(dv, el))
            implicit_submit(dv, el);
        au_node_unref(&el->node);
        return true;
    case AU_KEY_ESCAPE:
        if (is_html(el, AU_ATOM_input) && au_input_type(el) == AU_INPUT_SEARCH && c->value.len) {
            au_edit_select_all(c);
            delete_text(dv, el, -1, false, false);
            return true;
        }
        focus_el(dv, NULL, false, false);
        return true;
    case AU_KEY_PAGE_UP:
    case AU_KEY_PAGE_DOWN:
        if (multi) {
            float lh = 16;
            int lines = 10;
            (void)lh;
            uint32_t to = au_layout_field_vmove(dv->layout, el, c->caret,
                                                ev->key == AU_KEY_PAGE_UP ? -lines : lines, &dv->goal_x);
            au_edit_move_to(c, to, shift);
            moved(dv, el);
            return true;
        }
        return false;
    default:
        break;
    }
    if (cmd && (ev->ch == 'a' || ev->ch == 'A')) {
        au_edit_select_all(c);
        moved(dv, el);
        return true;
    }
    if (ev->ch >= 0x20 && ev->ch != 0x7F && !cmd && !(ev->mods & AU_MOD_CONTROL)) {
        char u[4];
        int n = au_utf8_encode(ev->ch, u);
        insert_text(dv, el, u, (size_t)n, AU_EDIT_TYPING, "insertText");
        return true;
    }
    return false;
}

/* Keys in other focused controls. */
static bool control_key(AuDocView *dv, AuElement *el, const AuEvent *ev)
{
    bool space = ev->ch == ' ', ret = ev->key == AU_KEY_RETURN;
    int vdir = ev->key == AU_KEY_UP || ev->key == AU_KEY_LEFT ? -1 : ev->key == AU_KEY_DOWN || ev->key == AU_KEY_RIGHT ? 1 : 0;
    if (is_html(el, AU_ATOM_a) || is_html(el, AU_ATOM_area)) {
        if (ret) {
            const char *href = get_attr(el, "href");
            char abs[2048];
            if (href && au_url_resolve(dv->base, href, abs, sizeof abs)) {
                if (starts_ci(abs, "javascript:")) {
                    if (dv->script)
                        au_docscript_javascript_url(dv->script, abs);
                } else if (dv->cb.open_url) {
                    const char *target = get_attr(el, "target");
                    bool nt = (ev->mods & AU_MOD_COMMAND) || (target && au_ieq(target, strlen(target), "_blank", 6));
                    dv->cb.open_url(dv->cb.ctx, dv, abs, nt);
                }
            }
            return true;
        }
        return false;
    }
    if (is_html(el, AU_ATOM_select)) {
        if (au_select_is_listbox(el)) {
            if (!vdir || ev->key == AU_KEY_LEFT || ev->key == AU_KEY_RIGHT)
                return false;
            AuControl *c = au_ctl(el);
            size_t n;
            AuElement **rows = au_select_options(el, true, &n);
            int r = c->cursor;
            if (r < 0) {
                int idx = au_select_index(el);
                /* the selected option's row */
                for (int i = 0, oi = 0; i < (int)n; i++) {
                    if (is_html(rows[i], AU_ATOM_optgroup))
                        continue;
                    if (oi++ == idx)
                        r = i;
                }
            }
            int k = r;
            for (k = r + vdir; k >= 0 && k < (int)n; k += vdir)
                if (!is_html(rows[k], AU_ATOM_optgroup) && !au_form_disabled(rows[k]))
                    break;
            au_free(rows);
            if (k >= 0 && k < (int)n) {
                bool extend = (ev->mods & AU_MOD_SHIFT) != 0;
                if (extend && c->cursor < 0)
                    c->cursor = r;
                listbox_pick(dv, el, k, false, extend);
                if (extend)
                    c->cursor = c->cursor; /* the anchor stays */
            }
            return true;
        }
        if (space || ret || (vdir && (ev->mods & AU_MOD_OPTION))) {
            select_popup(dv, el);
            return true;
        }
        if (vdir) {
            select_step(dv, el, vdir);
            return true;
        }
        return false;
    }
    if (is_html(el, AU_ATOM_input)) {
        AuInputType t = au_input_type(el);
        if (t == AU_INPUT_RANGE) {
            double mn = range_num(el, "min", 0), mx = range_num(el, "max", 100), st = range_num(el, "step", 1);
            if (st <= 0)
                st = 1;
            size_t n;
            double v = strtod(au_ctl_value(el, &n), NULL);
            if (vdir)
                v += vdir * st;
            else if (ev->key == AU_KEY_HOME)
                v = mn;
            else if (ev->key == AU_KEY_END)
                v = mx;
            else
                return false;
            range_set(dv, el, v, true);
            return true;
        }
        if (t == AU_INPUT_RADIO && vdir) {
            size_t n;
            AuElement **g = au_form_radio_group(el, &n);
            size_t at = 0;
            for (size_t i = 0; i < n; i++)
                if (g[i] == el)
                    at = i;
            AuElement *to = NULL;
            for (size_t k = 1; k < n && !to; k++) {
                AuElement *e = g[(at + n + (size_t)(vdir * (int)k) % n) % n];
                if (!au_form_disabled(e) && rendered(e))
                    to = e;
            }
            au_free(g);
            if (to) {
                focus_el(dv, to, true, false);
                synthetic_click(dv, to);
            }
            return true;
        }
        if (t == AU_INPUT_CHECKBOX || t == AU_INPUT_RADIO) {
            if (space) {
                synthetic_click(dv, el);
                return true;
            }
            if (ret) {
                AuElement *form = au_form_owner(el);
                if (form)
                    implicit_submit(dv, el);
                return true;
            }
            return false;
        }
        if (t == AU_INPUT_SUBMIT || t == AU_INPUT_RESET || t == AU_INPUT_BUTTON || t == AU_INPUT_IMAGE ||
            t == AU_INPUT_FILE || t == AU_INPUT_COLOR) {
            if (space || ret) {
                synthetic_click(dv, el);
                return true;
            }
        }
        return false;
    }
    if (is_html(el, AU_ATOM_button) || is_html(el, AU_ATOM_summary)) {
        if (space || ret) {
            synthetic_click(dv, el);
            return true;
        }
    }
    return false;
}

/* KeyboardEvent key and code for a key event. */
static void key_names(const AuEvent *ev, char *key, size_t kcap, char *code, size_t ccap)
{
    static const char *const names[] = {"",         "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
                                        "PageUp",   "PageDown",  "Home",       "End",     "Backspace",
                                        "Delete",   "Enter",     "Tab",        "Escape"};
    if (ev->key > 0 && ev->key < (int)(sizeof names / sizeof names[0])) {
        snprintf(key, kcap, "%s", names[ev->key]);
        snprintf(code, ccap, "%s", names[ev->key]);
        return;
    }
    char u[5] = {0};
    au_utf8_encode(ev->ch, u);
    snprintf(key, kcap, "%s", u);
    uint32_t c = ev->ch;
    if (c >= 'a' && c <= 'z')
        snprintf(code, ccap, "Key%c", (char)(c - 32));
    else if (c >= 'A' && c <= 'Z')
        snprintf(code, ccap, "Key%c", (char)c);
    else if (c >= '0' && c <= '9')
        snprintf(code, ccap, "Digit%c", (char)c);
    else if (c == ' ')
        snprintf(code, ccap, "Space");
    else
        snprintf(code, ccap, "%s", "");
}

bool au_docview_key(AuDocView *dv, const AuEvent *ev)
{
    if (!dv->doc || ev->type != AU_EVENT_KEY_DOWN)
        return false;
    bool in_frame;
    if (dv->focus_frame && au_frames_key(dv, ev, &in_frame)) {
        if (!dv->parent)
            au_frames_idle(dv);
        return in_frame;
    }
    AuElement *el = dv->focus && alive(dv, dv->focus) ? dv->focus : NULL;
    if (dv->focus && !el)
        focus_el(dv, NULL, false, true); /* the focused element went away */
    /* Command keys other than editing movement belong to the menus. */
    bool cmd = (ev->mods & AU_MOD_COMMAND) != 0;
    if (cmd && ev->ch && ev->key == AU_KEY_NONE && !(ev->ch == 'a' || ev->ch == 'A'))
        return false;
    bool used_by_script = false;
    dv->in_key = true;
    if (dv->script) {
        char key[16], code[24];
        key_names(ev, key, sizeof key, code, sizeof code);
        AuElement *target = el;
        if (target)
            au_node_ref(&target->node);
        bool ok = au_docscript_key(dv->script, target, "keydown", key, code, ev->mods);
        /* keypress goes to character keys and to Enter (code 13), as in
         * browsers; scripts submit on it. */
        if (ok && (ev->ch >= 0x20 || ev->key == AU_KEY_RETURN) && !cmd && dv->doc)
            ok = au_docscript_key(dv->script, target && alive(dv, target) ? target : NULL, "keypress", key, code,
                                  ev->mods);
        if (target)
            au_node_unref(&target->node);
        if (!dv->doc) {
            dv->in_key = false;
            return true;
        }
        el = dv->focus && alive(dv, dv->focus) ? dv->focus : NULL;
        if (!ok)
            used_by_script = true;
    }
    bool used = used_by_script;
    if (!used_by_script) {
        if (ev->key == AU_KEY_TAB && !cmd) {
            AuElement *to = next_tab(dv, el, (ev->mods & AU_MOD_SHIFT) != 0);
            if (to) {
                focus_el(dv, to, true, false);
                if (dv->focus == to) {
                    reveal_element(dv, to);
                    if (au_ctl_is_text_field(to))
                        au_layout_field_reveal(dv->layout, to);
                }
            }
            used = true;
        } else if (el && au_ctl_is_text_field(el)) {
            used = field_key(dv, el, ev);
        } else if (el) {
            used = control_key(dv, el, ev);
        }
    }
    if (dv->script && dv->doc) {
        char key[16], code[24];
        key_names(ev, key, sizeof key, code, sizeof code);
        AuElement *t = dv->focus && alive(dv, dv->focus) ? dv->focus : NULL;
        au_docscript_key(dv->script, t, "keyup", key, code, ev->mods);
    }
    dv->in_key = false;
    if (dv->restyle_needed || dv->tree_dirty || dv->sheets_dirty || dv->sheets_rescan) {
        /* What listeners and state changes did during the key
         * (au_dv_turn_end leaves it to here): update now, and repaint the
         * page only if the update changed something. A restyle that
         * computes the same values repaints nothing. A change still
         * waiting for its paint (pending) was invalidated when it was
         * made. */
        bool heavy = dv->tree_dirty || dv->sheets_dirty || dv->sheets_rescan;
        bool pending = au_layout_needs_paint(dv->layout);
        if (au_docview_update(dv) || heavy || (!pending && au_layout_needs_paint(dv->layout)))
            au_dv_invalidate_all(dv);
    }
    return used;
}

/* ---- Edit commands ---- */

bool au_docview_can_edit(const AuDocView *dv, int cmd)
{
    if (!au_docview_editing(dv))
        return false;
    AuControl *c = au_ctl_peek(dv->focus);
    bool sel = c && c->anchor != c->caret;
    bool pw = is_html(dv->focus, AU_ATOM_input) && au_input_type(dv->focus) == AU_INPUT_PASSWORD;
    bool ro = readonly(dv->focus);
    switch (cmd) {
    case AU_EDITCMD_UNDO: return c && c->has_undo && !ro;
    case AU_EDITCMD_CUT: return sel && !pw && !ro;
    case AU_EDITCMD_COPY: return sel && !pw;
    case AU_EDITCMD_CLEAR: return sel && !ro;
    case AU_EDITCMD_PASTE: return !ro;
    case AU_EDITCMD_SELECT_ALL: return true;
    default: return false;
    }
}

bool au_docview_edit(AuDocView *dv, int cmd, const char *text, size_t len, AuBuf *out)
{
    if (!au_docview_editing(dv))
        return false;
    AuElement *el = dv->focus;
    AuControl *c = au_ctl(el);
    au_ctl_value(el, NULL);
    bool pw = is_html(el, AU_ATOM_input) && au_input_type(el) == AU_INPUT_PASSWORD;
    switch (cmd) {
    case AU_EDITCMD_SELECT_ALL:
        au_edit_select_all(c);
        moved(dv, el);
        return true;
    case AU_EDITCMD_COPY:
        if (pw || !au_edit_has_selection(c))
            return false;
        if (out)
            au_edit_selected_text(c, out);
        return true;
    case AU_EDITCMD_CUT:
        if (pw || !au_edit_has_selection(c) || readonly(el))
            return false;
        if (out)
            au_edit_selected_text(c, out);
        return delete_text(dv, el, -1, false, false);
    case AU_EDITCMD_CLEAR:
        if (!au_edit_has_selection(c))
            return false;
        return delete_text(dv, el, -1, false, false);
    case AU_EDITCMD_PASTE:
        return text && insert_text(dv, el, text, len, AU_EDIT_OTHER, "insertFromPaste");
    case AU_EDITCMD_UNDO:
        if (readonly(el) || !before_input(dv, el, "historyUndo", NULL))
            return false;
    {
        unsigned before = field_style_state(dv, el);
        if (!au_edit_undo(au_ctl(el)))
            return false;
        edited(dv, el, "historyUndo", NULL, before);
        return true;
    }
    default:
        return false;
    }
}

/* ---- Mouse ---- */

static AuElement *control_of(AuElement *e)
{
    for (; e; e = parent_el(e)) {
        if (e->ns != AU_NS_HTML)
            continue;
        AuAtom t = e->tag;
        if (t == AU_ATOM_input || t == AU_ATOM_textarea || t == AU_ATOM_select || t == AU_ATOM_button)
            return e;
    }
    return NULL;
}

bool au_docview_control_at(AuDocView *dv, int x, int y)
{
    if (dv->drag)
        return true;
    AuHitResult r;
    if (!dv->doc || !au_docview_hit_test(dv, x, y, &r))
        return false;
    AuElement *c = control_of(r.element);
    return c && !(is_html(c, AU_ATOM_input) && au_input_type(c) == AU_INPUT_HIDDEN);
}

/* Point (x, y) in document coordinates relative to el's border box. */
static void local_point(AuDocView *dv, AuElement *el, const AuEvent *ev, const AuHitResult *r, float *lx, float *ly)
{
    if (r && r->element == el) {
        *lx = r->local_x;
        *ly = r->local_y;
        return;
    }
    AuRect b;
    if (!au_layout_element_rect(dv->layout, el, &b)) {
        *lx = *ly = 0;
        return;
    }
    float ox = 0, oy = 0;
    if (is_fixed(el)) {
        ox = (float)dv->scroll_x;
        oy = (float)dv->scroll_y;
    }
    *lx = (float)ev->x - b.x - ox;
    *ly = (float)ev->y - b.y - oy;
}

static void field_click(AuDocView *dv, AuElement *el, const AuEvent *ev, const AuHitResult *r)
{
    AuControl *c = au_ctl(el);
    size_t len;
    const char *v = au_ctl_value(el, &len);
    float lx, ly;
    local_point(dv, el, ev, r, &lx, &ly);
    uint32_t off;
    if (!au_layout_field_offset_at(dv->layout, el, lx, ly, &off))
        off = (uint32_t)len;
    dv->drag_clicks = ev->clicks >= 3 ? 3 : ev->clicks == 2 ? 2 : 1;
    dv->goal_x = -1;
    bool pw = is_html(el, AU_ATOM_input) && au_input_type(el) == AU_INPUT_PASSWORD;
    if (dv->drag_clicks == 2 && !pw) {
        uint32_t a, b;
        au_edit_word_at(v, len, off, &a, &b);
        dv->drag_a = a;
        dv->drag_b = b;
        au_edit_select(c, a, b);
    } else if (dv->drag_clicks >= 2) {
        uint32_t a = 0, b = (uint32_t)len;
        if (!au_ctl_is_single_line(el))
            au_edit_line_at(v, len, off, &a, &b);
        dv->drag_a = a;
        dv->drag_b = b;
        au_edit_select(c, a, b);
    } else {
        au_edit_move_to(c, off, (ev->mods & AU_MOD_SHIFT) != 0);
        dv->drag_a = dv->drag_b = c->anchor;
    }
    set_ref(&dv->drag, el);
    moved(dv, el);
}

static void field_drag(AuDocView *dv, AuElement *el, const AuEvent *ev)
{
    AuControl *c = au_ctl(el);
    size_t len;
    const char *v = au_ctl_value(el, &len);
    float lx, ly;
    local_point(dv, el, ev, NULL, &lx, &ly);
    uint32_t off;
    if (!au_layout_field_offset_at(dv->layout, el, lx, ly, &off))
        return;
    if (dv->drag_clicks == 2) {
        uint32_t a, b;
        au_edit_word_at(v, len, off, &a, &b);
        if (off < dv->drag_a)
            au_edit_select(c, dv->drag_b, a);
        else
            au_edit_select(c, dv->drag_a, b > dv->drag_b ? b : dv->drag_b);
    } else if (dv->drag_clicks >= 3) {
        uint32_t a = 0, b = (uint32_t)len;
        if (!au_ctl_is_single_line(el))
            au_edit_line_at(v, len, off, &a, &b);
        if (off < dv->drag_a)
            au_edit_select(c, dv->drag_b, a);
        else
            au_edit_select(c, dv->drag_a, b > dv->drag_b ? b : dv->drag_b);
    } else {
        au_edit_move_to(c, off, true);
    }
    moved(dv, el);
}

/* The element a press focuses: the nearest focusable ancestor of the
 * target (a label's control when it is a text field or select). */
static AuElement *focus_target(AuElement *e)
{
    for (; e; e = parent_el(e)) {
        if (focusable(e))
            return e;
        if (is_html(e, AU_ATOM_label)) {
            AuElement *c = label_control(e);
            if (c && (au_ctl_is_text_field(c) || is_html(c, AU_ATOM_select)) && focusable(c))
                return c;
        }
    }
    return NULL;
}

bool au_forms_mouse(AuDocView *dv, const AuEvent *ev, const AuHitResult *r)
{
    switch (ev->type) {
    case AU_EVENT_MOUSE_DOWN: {
        AuElement *ft = focus_target(r->element);
        if (ft != dv->focus)
            focus_el(dv, ft, false, false);
        if (!dv->doc)
            return true;
        AuElement *ctl = control_of(r->element);
        if (!ctl || !alive(dv, ctl) || au_form_disabled(ctl))
            return ctl != NULL;
        if (au_ctl_is_text_field(ctl)) {
            if (dv->focus == ctl)
                field_click(dv, ctl, ev, r);
            return true;
        }
        if (is_html(ctl, AU_ATOM_select)) {
            if (au_select_is_listbox(ctl)) {
                float lx, ly;
                local_point(dv, ctl, ev, r, &lx, &ly);
                int row = au_layout_listbox_row_at(dv->layout, ctl, lx, ly);
                if (row >= 0)
                    listbox_pick(dv, ctl, row, (ev->mods & AU_MOD_COMMAND) != 0, (ev->mods & AU_MOD_SHIFT) != 0);
                set_ref(&dv->drag, ctl);
            } else {
                select_popup(dv, ctl);
            }
            return true;
        }
        if (is_html(ctl, AU_ATOM_input) && au_input_type(ctl) == AU_INPUT_RANGE) {
            float lx, ly;
            local_point(dv, ctl, ev, r, &lx, &ly);
            float f = au_layout_range_frac_at(dv->layout, ctl, lx);
            if (f >= 0) {
                double mn = range_num(ctl, "min", 0), mx = range_num(ctl, "max", 100);
                range_set(dv, ctl, mn + f * (mx - mn), false);
            }
            set_ref(&dv->drag, ctl);
            return true;
        }
        /* buttons: painted pressed while held */
        set_ref(&dv->pressed, ctl);
        AuControl *c = au_ctl(ctl);
        c->pressed = true;
        au_dv_invalidate_element(dv, ctl);
        return true;
    }
    case AU_EVENT_MOUSE_MOVE: {
        AuElement *d = dv->drag;
        if (!d || !alive(dv, d))
            return false;
        if (au_ctl_is_text_field(d)) {
            field_drag(dv, d, ev);
        } else if (is_html(d, AU_ATOM_input)) {
            float lx, ly;
            local_point(dv, d, ev, NULL, &lx, &ly);
            float f = au_layout_range_frac_at(dv->layout, d, lx);
            if (f >= 0) {
                double mn = range_num(d, "min", 0), mx = range_num(d, "max", 100);
                range_set(dv, d, mn + f * (mx - mn), false);
            }
        } else if (is_html(d, AU_ATOM_select) && au_element_get_attr(d, AU_ATOM_multiple)) {
            float lx, ly;
            local_point(dv, d, ev, NULL, &lx, &ly);
            int row = au_layout_listbox_row_at(dv->layout, d, lx, ly);
            AuControl *c = au_ctl(d);
            if (row >= 0 && row != c->cursor) {
                int keep = c->cursor;
                listbox_pick(dv, d, row, false, true);
                c->cursor = keep;
            }
        }
        return true;
    }
    case AU_EVENT_MOUSE_UP: {
        bool was = dv->drag != NULL;
        AuElement *d = dv->drag;
        if (d && is_html(d, AU_ATOM_input) && au_input_type(d) == AU_INPUT_RANGE && alive(dv, d))
            fire(dv, d, "change", true, false);
        set_ref(&dv->drag, NULL);
        if (dv->pressed) {
            AuControl *c = au_ctl_peek(dv->pressed);
            if (c)
                c->pressed = false;
            au_dv_invalidate_element(dv, dv->pressed);
            set_ref(&dv->pressed, NULL);
        }
        return was;
    }
    default:
        return false;
    }
}
