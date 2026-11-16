/* Targeted invalidation of element state changes.
 *
 *   test_css_invalidate
 *       Runs the correctness tests: on pages with and without :has() rules,
 *       hover, active, focus and checked changes are restyled through
 *       au_css_invalidate_state / au_css_move_state, and every computed
 *       value of every element must equal what a full restyle gives.
 *
 *   test_css_invalidate --bench [--verify] PAGE.html [SHEET.css ...]
 *       Moves the hover over up to 300 elements of a page and prints how
 *       many elements each move restyles and how long it takes, first the
 *       way it worked before targeted invalidation (a full restyle when a
 *       :has() rule exists, else the subtrees of the two elements and of
 *       their following siblings), then targeted. --verify compares the
 *       first 40 targeted moves with a full restyle. */
#include "../../src/css/css_int.h"
#include "../../src/html/html.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint32_t css_resolver_take_computed(AuCssResolver *r);
bool css_resolver_has_has(AuCssResolver *r);

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static AuAtom prop_atoms[AU_PROP_LONGHAND_END];

static void init_props(void)
{
    au_css_init();
    for (int p = 1; p < AU_PROP_LONGHAND_END; p++)
        prop_atoms[p] = au_atom_cstr(au_css_property_name((AuCssProp)p));
}

static void dump_style(const AuStyle *s, AuBuf *out)
{
    if (!s) {
        au_buf_puts(out, "(none)\n");
        return;
    }
    for (int p = 1; p < AU_PROP_LONGHAND_END; p++) {
        au_style_serialize_property(s, prop_atoms[p], out);
        au_buf_putc(out, ';');
    }
    au_buf_printf(out, " f%u b%u\n", (unsigned)s->flags, (unsigned)s->pseudo_bits);
}

/* Every computed longhand of every element and of its ::before, ::after
 * and ::marker styles. */
static void dump_doc(AuDocument *doc, AuBuf *out)
{
    const AuNode *root = &doc->node;
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (n->type != AU_NODE_ELEMENT)
            continue;
        const AuStyle *s = au_element_style((AuElement *)n);
        dump_style(s, out);
        for (const AuStyle *p = s ? s->pseudos : NULL; p; p = p->next) {
            au_buf_printf(out, " ::%u ", (unsigned)p->pseudo);
            dump_style(p, out);
        }
    }
}

static AuElement **elements(AuDocument *doc, uint32_t *count)
{
    const AuNode *root = &doc->node;
    uint32_t n = 0;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        n += c->type == AU_NODE_ELEMENT;
    AuElement **v = au_alloc((n ? n : 1) * sizeof *v);
    n = 0;
    for (AuNode *c = root->first_child; c; c = au_node_next(c, root))
        if (c->type == AU_NODE_ELEMENT)
            v[n++] = (AuElement *)c;
    *count = n;
    return v;
}

/* Restyles, then checks the result against a full restyle. Returns the
 * number of elements the targeted restyle computed. */
static uint32_t restyle_and_compare(AuCssResolver *r, AuDocument *doc, const char *what, int step)
{
    css_resolver_take_computed(r);
    au_css_restyle(r, doc);
    uint32_t computed = css_resolver_take_computed(r);
    AuBuf a, b;
    au_buf_init(&a);
    au_buf_init(&b);
    dump_doc(doc, &a);
    au_css_style_document(r, doc);
    dump_doc(doc, &b);
    checks++;
    if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0) {
        failures++;
        fprintf(stderr, "%s step %d: targeted restyle differs from a full restyle\n", what, step);
        /* name the first differing element */
        const char *x = a.data, *y = b.data;
        size_t line = 0, i = 0;
        while (i < a.len && i < b.len && x[i] == y[i]) {
            line += x[i] == '\n';
            i++;
        }
        fprintf(stderr, "  first difference in style line %u\n", (unsigned)line);
        if (getenv("INVALIDATE_DUMP")) {
            size_t s = i > 60 ? i - 60 : 0;
            fprintf(stderr, "  targeted: %.*s\n  full:     %.*s\n", (int)(a.len - s < 160 ? a.len - s : 160), x + s,
                    (int)(b.len - s < 160 ? b.len - s : 160), y + s);
        }
    }
    au_buf_free(&a);
    au_buf_free(&b);
    return computed;
}

static AuElement *parent_of(AuElement *el)
{
    AuNode *p = el->node.parent;
    return p && p->type == AU_NODE_ELEMENT ? (AuElement *)p : NULL;
}

static void set_focus(AuCssResolver *r, AuElement *el, bool on, bool visible, bool generic)
{
    au_element_set_state(el, AU_STATE_FOCUS, on);
    au_element_set_state(el, AU_STATE_FOCUS_VISIBLE, on && visible);
    for (AuElement *p = parent_of(el); p; p = parent_of(p)) {
        if (generic)
            au_css_invalidate(r, p, AU_CSS_CHANGE_STATE, 0);
        else
            au_css_invalidate_state(r, p, AU_STATE_FOCUS_WITHIN);
    }
    if (generic)
        au_css_invalidate(r, el, AU_CSS_CHANGE_STATE, 0);
    else
        au_css_invalidate_state(r, el, AU_STATE_FOCUS | AU_STATE_FOCUS_VISIBLE | AU_STATE_FOCUS_WITHIN);
}

static uint32_t rng_state = 7;
static uint32_t rnd(uint32_t n)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) % n;
}

static const char page[] =
    "<!doctype html><html><head><title>t</title></head><body>"
    "<div id=top class='menu a'><ul class=list>"
    "<li class=item><a href=# class=link>one <b>bold</b></a><span class=tip>tip</span></li>"
    "<li class='item x'><a href=#>two</a><em>e</em></li>"
    "<li class=item><input type=checkbox id=c1><label for=c1>L <i>i</i></label>"
    "<p class=after>after <span>s</span></p></li>"
    "<li class=item><input type=checkbox id=c2 checked><label for=c2>M</label></li>"
    "</ul></div>"
    "<div class='card b'><h2>t</h2><p class=body>text <a href=# class=link>l</a></p>"
    "<div class=foot><input type=radio name=r id=r1><input type=radio name=r id=r2>"
    "<span class=mark>m</span><button>go</button></div></div>"
    "<div class='sib c'><span>1</span><span class=k>2</span><span>3</span><span>4</span></div>"
    "<p class=tail>tail <code>c</code></p>"
    "<table><tr><td>a</td><td class=k>b</td></tr><tr><td>c</td><td><a href=#>d</a></td></tr></table>"
    "</body></html>";

static const char sheet_plain[] =
    "a:hover { color: red; text-decoration: underline }"
    ".item:hover { background: #eee; --c: purple }"
    ".tip { color: var(--c, black) }"
    ".item:hover .tip { display: block; font-weight: bold }"
    ".menu:hover > .list { border: 1px solid red }"
    "li:hover + li { margin-top: 3px }"
    "li:hover ~ li a { font-style: italic }"
    ".card:hover { font-size: 20px }"
    ".body { margin-left: 1em }"
    ".card:hover .foot span { width: 2em }"
    ":not(:hover) > b { opacity: .5 }"
    ":is(.card, .sib):hover span { color: green }"
    "span:hover::before { content: 'x' }"
    ".k:not(:hover) { height: 5px }"
    ".list:hover > li:first-child { padding: 1px }"
    "tr:hover td { background: yellow }"
    "td:hover + td { color: blue }"
    "body:hover .tail { color: gray }"
    "html:hover { scrollbar-color: red blue }"
    ".card { &:hover { border: 2px solid } & .foot:hover .mark { color: red } }"
    "input:checked + label { color: red }"
    "input:checked ~ p span { color: blue }"
    "input:checked { outline: 2px solid red }"
    "li:not(.x) > input:not(:checked) + label i { color: teal }"
    ":focus { outline: 1px solid blue }"
    ".card:focus-within { background: yellow }"
    ".card:focus-within h2 { color: red }"
    "a:focus-visible { outline-style: dotted }"
    "a:focus + span { color: navy }"
    "a:active { color: orange }"
    ".menu:active .tip { color: pink }"
    "button:active { padding: 3px } button:hover:not(:active) { margin: 2px }"
    /* classes, ids and attributes */
    ".on { color: red; --c: teal } .on > b { font-weight: 900 } .on + li { top: 1px } .on ~ * span { left: 1px }"
    "[data-s] { right: 2px } [data-s=b] .tip { bottom: 1px } [data-s]::before { content: attr(data-s) }"
    "#zap { outline: 1px solid } #zap + div span { color: aqua } :not(.on) > em { padding: 1px }"
    ":is(.on, #zap) > a { letter-spacing: 2px } .card.on .foot > .mark.on { color: lime }"
    "[style] > span { word-spacing: 1px } a:link { color: navy } :lang(fr) > code { color: brown }"
    "input:disabled + label { color: gray } [class~=on] i { color: silver } [id] > h2 { margin: 3px }"
    "td[bgcolor] + td { color: white }";

static const char sheet_has_plain[] = /* :has() without a state inside */
    ".card:has(.foot) { margin: 4px }"
    "li:has(> input) { padding: 2px }"
    ".menu:has(.tip) .link { letter-spacing: 1px }";

static const char sheet_has_state[] =
    ".card:has(a:hover) { background: pink }"
    ".menu:has(.item:hover) .tip { text-transform: uppercase }"
    "li:has(> input:checked) { background: lime }"
    "li:has(input:checked) label i { color: red }"
    ".sib:has(.k:hover) span { width: 9px }"
    "span:has(+ .k:hover) { color: red }"
    "span:has(~ span:hover) { margin-left: 2px }"
    "div:has(~ p code:hover) { border-left: 3px solid }"
    "div:has(+ div :hover) > h2 { word-spacing: 2px }"
    "body:has(:focus) { padding: 1px }"
    ".card:has(:focus-within) h2 { letter-spacing: 1px }"
    ".card:has(:focus-visible) + div { top: 1px }"
    ".menu:has(a:active) { opacity: .9 }"
    ":not(:has(:hover)) > em { color: teal }"
    ".card:has(:is(.foot :hover)) { margin: 1px }"
    "html:has(.tail:hover) { background: #fafafa }"
    "tr:has(td:hover) + tr a { color: maroon }"
    "table:has(a:hover) td.k { color: olive }"
    ".foot:has(#r1:checked) .mark { color: red }"
    ".foot:has(#r2:checked) { --c: blue } .mark { background: var(--c, white) }"
    /* classes, ids and attributes in :has() arguments */
    ".menu:has(.on) .tip { text-indent: 2px } div:has(> [data-s]) { min-width: 3px }"
    "body:has(#zap) { margin: 9px } span:has(+ .on) { color: fuchsia } .card:has(.foot .on) h2 { top: 2px }"
    "li:has(~ li.on) a { bottom: 3px } :not(:has([data-s])) > code { color: tan }"
    "table:has(td[bgcolor]) { border-spacing: 1px } ul:has(a:link) { padding: 4px }";

static void run_page(const char *what, const char *css1, const char *css2, uint32_t *max_hover)
{
    const char *label = what;
    AuDocument *doc = au_html_parse(page, sizeof page - 1, NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, au_css_sheet_parse(css1, strlen(css1), AU_CSS_ORIGIN_AUTHOR, "http://x/", NULL));
    if (css2)
        au_css_resolver_add_sheet(r, au_css_sheet_parse(css2, strlen(css2), AU_CSS_ORIGIN_AUTHOR, "http://x/", NULL));
    au_css_style_document(r, doc);
    uint32_t n;
    AuElement **el = elements(doc, &n);
    uint32_t biggest = 0;

    /* hover: every element in order, then jumps, then out of the page;
     * the second half compares only every fifth move, so targeted
     * restyles follow one another */
    AuElement *cur = NULL;
    int step = 0;
    for (uint32_t i = 0; i < 5 * n + 1; i++) {
        AuElement *t = i < n ? el[i] : i == 5 * n ? NULL : el[rnd(n)];
        au_css_move_state(r, cur, t, AU_STATE_HOVER);
        cur = t;
        if (i < 2 * n || i % 5 == 0 || i == 5 * n) {
            uint32_t c = restyle_and_compare(r, doc, what, step);
            if (c > biggest)
                biggest = c;
        } else {
            au_css_restyle(r, doc);
        }
        step++;
    }
    if (max_hover)
        *max_hover = biggest;

    /* active, with the hover on another element */
    AuElement *act = NULL;
    cur = NULL;
    for (uint32_t i = 0; i < 2 * n + 1; i++) {
        AuElement *t = i == 2 * n ? NULL : el[rnd(n)];
        if (i % 3 == 0) {
            au_css_move_state(r, cur, el[rnd(n)], AU_STATE_HOVER);
            cur = NULL;
            for (uint32_t k = 0; k < n; k++) /* the deepest hovered element */
                if (el[k]->state & AU_STATE_HOVER)
                    cur = el[k];
        }
        au_css_move_state(r, act, t, AU_STATE_ACTIVE);
        act = t;
        restyle_and_compare(r, doc, what, step++);
    }
    au_css_move_state(r, cur, NULL, AU_STATE_HOVER);
    restyle_and_compare(r, doc, what, step++);

    /* focus: by bits, and the way callers without the bits report it */
    AuElement *foc = NULL;
    for (uint32_t i = 0; i < 3 * n + 1; i++) {
        AuElement *t = i < n ? el[i] : i == 3 * n ? NULL : el[rnd(n)];
        bool generic = i % 2 == 1;
        if (foc)
            set_focus(r, foc, false, false, generic);
        if (t)
            set_focus(r, t, true, i % 3 == 0, generic);
        foc = t;
        restyle_and_compare(r, doc, what, step++);
    }

    /* checked */
    for (uint32_t i = 0; i < 4 * n; i++) {
        AuElement *t = el[i % n];
        if (t->tag != au_atom_cstr("input"))
            continue;
        bool on = (i / n) % 2 == 0;
        au_element_set_state(t, AU_STATE_CHECKED_SET, true);
        au_element_set_state(t, AU_STATE_CHECKED, on);
        if (i % 2)
            au_css_invalidate(r, t, AU_CSS_CHANGE_STATE, 0);
        else
            au_css_invalidate_state(r, t, AU_STATE_CHECKED | AU_STATE_CHECKED_SET);
        restyle_and_compare(r, doc, what, step++);
    }

    /* classes, ids, attributes and inline styles: some named by selectors,
     * some not; reported before the change (as the DOM hook does) or
     * after it */
    AuAtom data_s = au_atom_cstr("data-s"), data_u = au_atom_cstr("data-u"), a_href = au_atom_cstr("href");
    AuAtom a_lang = au_atom_cstr("lang"), a_dis = au_atom_cstr("disabled"), a_bg = au_atom_cstr("bgcolor");
    for (uint32_t i = 0; i < 12 * n; i++) {
        AuElement *t = el[rnd(n)];
        bool before = rnd(2) == 0;
        uint32_t kind = rnd(12);
        AuAtom attr = 0;
        const char *val = NULL; /* NULL removes */
        char cls[64];
        switch (kind) {
        case 0:
        case 1:
        case 2: { /* toggle a class selectors name, or one they do not */
            const char *name = kind == 2 ? "zz" : "on";
            AuAtom ca = au_atom_cstr(name);
            const char *cur = au_element_get_attr(t, AU_ATOM_class);
            attr = AU_ATOM_class;
            if (au_element_has_class(t, ca)) {
                /* drop it: keep the first other class */
                cls[0] = 0;
                for (uint32_t k = 0; k < t->nclasses; k++)
                    if (t->classes[k] != ca && strlen(cls) + strlen(au_atom_str(t->classes[k])) + 2 < sizeof cls) {
                        strcat(cls, au_atom_str(t->classes[k]));
                        strcat(cls, " ");
                    }
            } else {
                snprintf(cls, sizeof cls, "%.40s %s", cur ? cur : "", name);
            }
            val = cls;
            break;
        }
        case 3: attr = data_s; val = rnd(3) == 0 ? NULL : rnd(2) ? "a" : "b"; break;
        case 4: attr = data_u; val = rnd(2) ? "x" : NULL; break;
        case 5: attr = AU_ATOM_id; val = rnd(2) ? "zap" : rnd(2) ? "other" : NULL; break;
        case 6: attr = AU_ATOM_style; val = rnd(3) == 0 ? NULL : rnd(2) ? "color: green; font-size: 12px" : "top: 4px"; break;
        case 7: attr = a_href; val = rnd(2) ? "#x" : NULL; break;
        case 8: attr = a_lang; val = rnd(2) ? "fr" : NULL; break;
        case 9: attr = a_dis; val = rnd(2) ? "" : NULL; break;
        case 10: attr = a_bg; val = rnd(2) ? "#ff0000" : NULL; break;
        default: attr = au_atom_cstr("aria-expanded"); val = rnd(2) ? "true" : "false"; break;
        }
        AuCssChange ch = attr == AU_ATOM_style ? AU_CSS_CHANGE_INLINE_STYLE : AU_CSS_CHANGE_ATTR;
        if (before)
            au_css_invalidate(r, t, ch, attr);
        if (val)
            au_element_set_attr(t, attr, val, strlen(val));
        else
            au_element_remove_attr(t, attr);
        if (!before)
            au_css_invalidate(r, t, ch, attr);
        if (i % 3 != 2) /* sometimes two changes before one restyle */
            restyle_and_compare(r, doc, label, step);
        step++;
    }
    restyle_and_compare(r, doc, what, step++);

    au_free(el);
    au_css_clear_document(doc);
    au_node_unref(&doc->node);
    au_css_resolver_free(r);
}

/* A state change restyles few elements when no rule reaches further. */
static void test_counts(void)
{
    static const char html[] =
        "<!doctype html><body><div class=card><p>a <a href=# id=l1>one</a> b <a href=# id=l2>two</a></p>"
        "<ul><li>1<li>2<li>3<li>4<li>5<li>6<li>7<li>8</ul></div><p id=q>tail <b>x</b></p></body>";
    static const char css[] = "a:hover { color: red } .card:has(ul) { margin: 1px } p:hover { background: #eee }"
                              " li:hover { color: blue } .noop { margin-left: 0 } .tint { background-color: #abc !important }";
    AuDocument *doc = au_html_parse(html, sizeof html - 1, NULL);
    AuCssResolver *r = au_css_resolver_new();
    au_css_resolver_add_sheet(r, au_css_sheet_parse(css, strlen(css), AU_CSS_ORIGIN_AUTHOR, "http://x/", NULL));
    au_css_style_document(r, doc);
    CHECK(css_resolver_has_has(r));
    uint32_t n;
    AuElement **el = elements(doc, &n);
    AuElement *l1 = NULL, *l2 = NULL, *q = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (el[i]->id == au_atom_cstr("l1"))
            l1 = el[i];
        if (el[i]->id == au_atom_cstr("l2"))
            l2 = el[i];
        if (el[i]->id == au_atom_cstr("q"))
            q = el[i];
    }
    CHECK(l1 && l2 && q);
    css_resolver_take_computed(r);
    au_css_move_state(r, NULL, l1, AU_STATE_HOVER);
    au_css_restyle(r, doc);
    /* the link and its paragraph, the ancestors a :hover rule can match;
     * p changes, so its other link is restyled too */
    uint32_t c = css_resolver_take_computed(r);
    CHECK(c == 3);
    CHECK(l1->state & AU_STATE_HOVER);
    CHECK(parent_of(l1)->state & AU_STATE_HOVER);
    au_css_move_state(r, l1, l2, AU_STATE_HOVER);
    au_css_restyle(r, doc);
    c = css_resolver_take_computed(r);
    CHECK(c == 2); /* the two links; their paragraph keeps the hover */
    CHECK(!(l1->state & AU_STATE_HOVER) && (l2->state & AU_STATE_HOVER));
    au_css_move_state(r, l2, q, AU_STATE_HOVER);
    au_css_restyle(r, doc);
    c = css_resolver_take_computed(r);
    /* a, p (changed: its two links and text restyle), div, and #q with its child */
    CHECK(c <= 8);
    CHECK(!(l2->state & AU_STATE_HOVER) && (q->state & AU_STATE_HOVER));
    /* a state no rule reads restyles nothing */
    au_element_set_state(q, AU_STATE_TARGET, true);
    au_css_invalidate_state(r, q, AU_STATE_TARGET);
    au_css_restyle(r, doc);
    CHECK(css_resolver_take_computed(r) == 0);
    /* nor does an attribute or a class no selector names, though the
     * page has :has() rules; reported before the change, as the DOM hook
     * does, or after it */
    uint32_t before = au_css_resolver_styled_count(r);
    au_css_invalidate(r, l1, AU_CSS_CHANGE_ATTR, au_atom_cstr("aria-expanded"));
    au_element_set_attr(l1, au_atom_cstr("aria-expanded"), "true", 4);
    au_css_invalidate(r, q, AU_CSS_CHANGE_ATTR, au_atom_cstr("value"));
    au_element_set_attr(q, au_atom_cstr("value"), "typed", 5);
    au_css_restyle(r, doc);
    CHECK(au_css_resolver_styled_count(r) == before);
    au_css_invalidate(r, l2, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_element_set_attr(l2, AU_ATOM_class, "unnamed other", 13);
    au_element_set_attr(q, AU_ATOM_class, "unnamed", 7);
    au_css_invalidate(r, q, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_css_restyle(r, doc);
    CHECK(au_css_resolver_styled_count(r) == before);
    /* a class a subject compound names restyles that element only, and a
     * style that comes out with the same values keeps its object */
    const AuStyle *q_style = au_element_style(q), *l2_style = au_element_style(l2);
    const AuStyle *b_style = au_element_style((AuElement *)q->node.last_child);
    au_css_invalidate(r, q, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_element_set_attr(q, AU_ATOM_class, "unnamed noop", 12); /* ".noop {}" matches, declares nothing new */
    au_css_restyle(r, doc);
    CHECK(au_css_resolver_styled_count(r) == before + 1);
    CHECK(au_element_style(q) == q_style);
    CHECK(au_element_style((AuElement *)q->node.last_child) == b_style);
    /* a real change replaces the element's style and keeps the groups
     * that did not change, and the styles of elements it does not reach */
    au_style_retain((AuStyle *)q_style); /* looked at after the element lets go of it */
    au_css_invalidate(r, q, AU_CSS_CHANGE_ATTR, AU_ATOM_class);
    au_element_set_attr(q, AU_ATOM_class, "tint", 4);
    au_css_restyle(r, doc);
    CHECK(au_element_style(q) != q_style);
    CHECK(au_element_style(q)->box == q_style->box && au_element_style(q)->surr == q_style->surr);
    CHECK(au_element_style(q)->inh == q_style->inh);
    CHECK(au_element_style(q)->bg != q_style->bg);
    au_style_release((AuStyle *)q_style);
    CHECK(au_element_style(l2) == l2_style);
    CHECK(au_element_style((AuElement *)q->node.last_child) == b_style);
    /* a full restyle with nothing changed keeps every style object */
    q_style = au_element_style(q);
    au_css_style_document(r, doc);
    CHECK(au_element_style(q) == q_style && au_element_style(l2) == l2_style);
    au_free(el);
    au_css_clear_document(doc);
    au_node_unref(&doc->node);
    au_css_resolver_free(r);
}

/* ---- Bench ---- */

/* ---- Viewport changes ----
 * After a change of the viewport size alone, the resolver keeps its rule
 * index when no media query changes its result and computes again only
 * the styles that use viewport units. Each step compares every computed
 * value with a second document styled from scratch by a new resolver at
 * that size. */

static const char sheet_viewport[] =
    "body { margin: 0 } .a { color: red }"
    "@media (min-width: 600px) { .item { padding: 4px } .x { display: block } }"
    "@media (min-width: 900px) and (max-height: 800px) { p { margin: 3px } }"
    "@media (orientation: portrait) { ul { border: 1px solid } }"
    "@media (min-aspect-ratio: 2/1) { #f { color: green } }"
    "@media (max-width: 40em) { h1 { font-size: 12px } }"
    "@media print { p { display: none } }";
static const char sheet_units[] =
    ":root { --gap: 2vw } #wrap { font-size: 2vw } #wrap > p { margin-left: 2em }"
    "ul { width: 50vw; padding: calc(1vh + 2px) } .item::before { content: 'x'; margin-right: 1vmin }"
    "#f { height: 10dvh; margin: var(--gap) } button { min-width: 5vmax } label { width: 10svw }";
static const char sheet_static[] = ".item { padding: 1px } p { margin: 2em } #wrap { color: blue }";

/* Blocks a lazy sheet must read as an eager one does: errors inside a
 * block, braces in custom properties, nesting, at-rules in blocks,
 * comments, strings holding braces, !important, an unclosed last block,
 * conditional groups that do and do not apply. */
static const char sheet_lazy[] =
    "p { color: red; colr: blue; margin: 1px 2px 3px 4px 5px; padding: 3px !important; ; }"
    "div { --x: { a: b; c: d }; --y: 4px; width: calc(var(--y) * 3); color: red }"
    "ul { content: '}'; quotes: \"{\" \"}\"; /* } */ margin-top: 7px }"
    "li { color: green; & span { color: blue } margin-left: 2px; @media (min-width: 600px) { padding: 9px } }"
    "span { color: ; top: 1px; left: (2px; right: 3px }"
    "a { color: red !important; color: blue; background: url(x.png) no-repeat, linear-gradient(red, blue) }"
    "h1 { } h2 {;;} h3 { color: } b { top: 1px"
    "} i { margin: 0 auto !ie; margin: 3px }"
    "@media screen and (max-width: 700px) { p { text-indent: 5px } @media (min-width: 400px) { p { top: 2px } } }"
    "@media print { p { text-indent: 50px } em { color: red } }"
    "@supports (display: grid) { em { letter-spacing: 1px } }"
    "@supports (display: nothing) { em { letter-spacing: 9px } }"
    "@layer a, b; @layer b { strong { color: red } } @layer a { strong { color: blue } }"
    "@media (min-width: 900px) { @layer a { strong { top: 4px } } label { margin: 1px } }"
    "em { width: 10px; { color: red } height: 5px } u { color: blue";

/* Named layers only: an earlier occurrence of such a sheet is left out of
 * the rule index but still names its layers first. */
static const char sheet_layers[] =
    "@layer x, y; @layer y { p { color: red !important; top: 1px } li { margin: 1px } }"
    "@layer x { p { color: blue !important; top: 2px } li { margin: 2px !important } } p { left: 3px }";
static const char sheet_layers_between[] =
    "@layer z { p { color: green !important; top: 5px } } @layer x { p { top: 7px; color: gray !important } }"
    "@layer y { li { margin: 9px !important } } p { left: 4px } @layer { li { padding: 1px } }";

typedef struct ViewDoc {
    AuDocument *doc;
    AuCssResolver *r;
} ViewDoc;

static char *read_file(const char *path, size_t *len);

/* Parses html and its <style> elements (plus extra sheets) into a styled
 * document at w x h. */
static ViewDoc view_doc(const char *html, size_t len, const char *const *extra, int nextra, float w, float h)
{
    ViewDoc v;
    v.doc = au_html_parse(html, len, NULL);
    v.r = au_css_resolver_new();
    uint32_t n;
    AuElement **el = elements(v.doc, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (el[i]->tag != au_atom_cstr("style"))
            continue;
        AuBuf t;
        au_buf_init(&t);
        for (AuNode *c = el[i]->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_TEXT)
                au_buf_append(&t, ((AuText *)c)->data, ((AuText *)c)->len);
        au_css_resolver_add_sheet(v.r, au_css_sheet_parse(t.data ? t.data : "", t.len, AU_CSS_ORIGIN_AUTHOR, NULL, NULL));
        au_buf_free(&t);
    }
    au_free(el);
    /* a text given again (the same pointer) shares the parsed sheet, as a
     * document does for a sheet it links twice */
    AuCssSheet *parsed[64];
    for (int i = 0; i < nextra; i++) {
        AuCssSheet *sh = NULL;
        for (int k = 0; k < i && k < 64 && !sh; k++)
            if (extra[k] == extra[i])
                sh = au_css_sheet_retain(parsed[k]);
        if (!sh)
            sh = au_css_sheet_parse(extra[i], strlen(extra[i]), AU_CSS_ORIGIN_AUTHOR, NULL, NULL);
        if (i < 64)
            parsed[i] = sh;
        au_css_resolver_add_sheet(v.r, sh);
    }
    AuCssDevice dev;
    au_css_device_default(&dev);
    dev.width = w;
    dev.height = h;
    au_css_resolver_set_device(v.r, &dev);
    au_css_style_document(v.r, v.doc);
    return v;
}

static void view_doc_free(ViewDoc *v)
{
    au_css_clear_document(v->doc);
    au_css_resolver_free(v->r);
    au_node_unref(&v->doc->node);
}

/* Resizes through the sizes; expect is the mode every step must report
 * (-1: any). Returns the largest number of elements one step computed. */
static uint32_t viewport_steps(const char *what, const char *html, size_t len, const char *const *extra, int nextra,
                               int expect, int *modes_seen)
{
    static const float sizes[][2] = {{1023, 768}, {1024, 767}, {800, 600},  {600, 600}, {599, 600},
                                     {600, 900},  {320, 480},  {1440, 900}, {1800, 700}, {1024, 100}, {641, 768},
                                     {640, 768},  {1024, 768}};
    ViewDoc v = view_doc(html, len, extra, nextra, 1024, 768);
    uint32_t total;
    au_free(elements(v.doc, &total));
    uint32_t most = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        AuCssDevice dev = *au_css_resolver_device(v.r);
        dev.width = sizes[i][0];
        dev.height = sizes[i][1];
        au_css_resolver_set_device(v.r, &dev);
        int mode = au_css_resolver_viewport_restyle(v.r);
        if (modes_seen)
            *modes_seen |= 1 << mode;
        css_resolver_take_computed(v.r);
        au_css_restyle(v.r, v.doc);
        uint32_t computed = css_resolver_take_computed(v.r);
        if (computed > most)
            most = computed;
        ViewDoc f = view_doc(html, len, extra, nextra, sizes[i][0], sizes[i][1]);
        AuBuf a, b;
        au_buf_init(&a);
        au_buf_init(&b);
        dump_doc(v.doc, &a);
        dump_doc(f.doc, &b);
        checks++;
        if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0) {
            failures++;
            size_t k = 0, line = 0;
            while (k < a.len && k < b.len && a.data[k] == b.data[k])
                line += a.data[k++] == '\n';
            fprintf(stderr, "%s at %gx%g (mode %d): styles differ from a fresh document, style line %u\n", what,
                    sizes[i][0], sizes[i][1], mode, (unsigned)line);
        }
        au_buf_free(&a);
        au_buf_free(&b);
        view_doc_free(&f);
        checks++;
        if ((mode == AU_CSS_VIEWPORT_NONE && computed != 0) || (expect >= 0 && mode != expect)) {
            failures++;
            fprintf(stderr, "%s at %gx%g: mode %d (expected %d), %u of %u elements computed\n", what, sizes[i][0],
                    sizes[i][1], mode, expect, computed, total);
        }
    }
    view_doc_free(&v);
    return most;
}

static int test_viewport(const char *samples)
{
    const char *none[] = {sheet_static};
    const char *units[] = {sheet_static, sheet_units};
    const char *media[] = {sheet_static, sheet_viewport};
    const char *both[] = {sheet_viewport, sheet_units};
    uint32_t total;
    AuDocument *d = au_html_parse(page, sizeof page - 1, NULL);
    au_free(elements(d, &total));
    au_node_unref(&d->node);
    /* nothing depends on the viewport: no element is ever computed */
    uint32_t most = viewport_steps("static", page, sizeof page - 1, none, 1, AU_CSS_VIEWPORT_NONE, NULL);
    CHECK(most == 0);
    /* viewport units only: the users and what inherits from them */
    most = viewport_steps("units", page, sizeof page - 1, units, 2, AU_CSS_VIEWPORT_UNITS, NULL);
    CHECK(most > 0 && most < total);
    printf("viewport units: at most %u of %u elements computed\n", most, total);
    /* media queries only: nothing between breakpoints, everything across one */
    int seen = 0;
    viewport_steps("media", page, sizeof page - 1, media, 2, -1, &seen);
    CHECK(seen == ((1 << AU_CSS_VIEWPORT_NONE) | (1 << AU_CSS_VIEWPORT_ALL)));
    seen = 0;
    viewport_steps("media+units", page, sizeof page - 1, both, 2, -1, &seen);
    CHECK(seen == ((1 << AU_CSS_VIEWPORT_UNITS) | (1 << AU_CSS_VIEWPORT_ALL)));
    /* the sample pages, with wiki.css for the page that links it */
    static const char *const names[] = {"wiki.html",     "hn.html",       "hn-item.html", "blog.html",   "docs.html",
                                        "forum.html",    "reddit.html",   "images.html",  "svg.html",    "emoji.html",
                                        "qa-boxes.html", "qa-edge.html",  "qa-flex.html", "qa-forms.html",
                                        "qa-inline.html", "qa-quirks.html", "qa-tables.html", "webfonts.html"};
    for (size_t i = 0; samples && i < sizeof names / sizeof names[0]; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", samples, names[i]);
        size_t len, clen = 0;
        char *html = read_file(path, &len);
        CHECK(html != NULL);
        if (!html)
            continue;
        char *css = NULL;
        if (!strcmp(names[i], "wiki.html")) {
            snprintf(path, sizeof path, "%s/wiki.css", samples);
            css = read_file(path, &clen);
            CHECK(css != NULL);
        }
        /* as written (the samples have no media queries and no viewport
         * units: nothing may be computed), then with sheets that add both */
        const char *extra[3] = {sheet_viewport, sheet_units, css};
        seen = 0;
        most = viewport_steps(names[i], html, len, extra + 2, css ? 1 : 0, AU_CSS_VIEWPORT_NONE, &seen);
        CHECK(most == 0);
        seen = 0;
        most = viewport_steps(names[i], html, len, extra, css ? 3 : 2, -1, &seen);
        CHECK(seen & (1 << AU_CSS_VIEWPORT_ALL));
        free(html);
        free(css);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

/* ---- Lazy sheets against eager ones ----
 * A sheet keeps blocks as text until they are used (css_lazy_bodies).
 * Styles computed from such sheets must equal styles from sheets parsed
 * whole: at the first size, after each resize, and with a hover moved
 * over the page (which matches rules for the first time). */
static void compare_docs(const char *what, const char *step, ViewDoc *a, ViewDoc *b)
{
    AuBuf x, y;
    au_buf_init(&x);
    au_buf_init(&y);
    dump_doc(a->doc, &x);
    dump_doc(b->doc, &y);
    checks++;
    if (x.len != y.len || memcmp(x.data, y.data, x.len) != 0) {
        failures++;
        size_t k = 0, line = 0;
        while (k < x.len && k < y.len && x.data[k] == y.data[k])
            line += x.data[k++] == '\n';
        fprintf(stderr, "%s, %s: lazy and eager sheets give different styles, style line %u\n", what, step,
                (unsigned)line);
        if (getenv("INVALIDATE_DUMP")) {
            size_t s0 = k > 80 ? k - 80 : 0;
            fprintf(stderr, "  lazy:  %.*s\n  eager: %.*s\n", (int)(x.len - s0 < 200 ? x.len - s0 : 200), x.data + s0,
                    (int)(y.len - s0 < 200 ? y.len - s0 : 200), y.data + s0);
        }
    }
    au_buf_free(&x);
    au_buf_free(&y);
}

static void lazy_page(const char *what, const char *html, size_t len, const char *const *extra, int nextra)
{
    css_lazy_bodies = true;
    ViewDoc lazy = view_doc(html, len, extra, nextra, 1024, 768);
    css_lazy_bodies = false;
    ViewDoc eager = view_doc(html, len, extra, nextra, 1024, 768);
    /* and with every occurrence of a repeated sheet in the rule index */
    css_index_skip_repeats = false;
    ViewDoc all = view_doc(html, len, extra, nextra, 1024, 768);
    css_index_skip_repeats = true;
    css_lazy_bodies = true;
    compare_docs(what, "first style", &lazy, &eager);
    compare_docs(what, "first style, every occurrence of a repeated sheet", &lazy, &all);
    view_doc_free(&all);
    static const float sizes[][2] = {{500, 700}, {1440, 900}, {320, 480}, {1024, 768}};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        ViewDoc *v[2] = {&lazy, &eager};
        for (int k = 0; k < 2; k++) {
            AuCssDevice dev = *au_css_resolver_device(v[k]->r);
            dev.width = sizes[i][0];
            dev.height = sizes[i][1];
            au_css_resolver_set_device(v[k]->r, &dev);
            au_css_restyle(v[k]->r, v[k]->doc);
        }
        char step[40];
        snprintf(step, sizeof step, "at %gx%g", sizes[i][0], sizes[i][1]);
        compare_docs(what, step, &lazy, &eager);
    }
    /* hover on a spread of elements, the same ones in both documents */
    uint32_t n, m;
    AuElement **a = elements(lazy.doc, &n), **b = elements(eager.doc, &m);
    CHECK(n == m);
    uint32_t steps = n < 24 ? n : 24;
    AuElement *ca = NULL, *cb = NULL;
    for (uint32_t s = 0; s < steps && n == m; s++) {
        uint32_t k = (uint32_t)((uint64_t)s * n / steps);
        au_css_move_state(lazy.r, ca, a[k], AU_STATE_HOVER);
        au_css_move_state(eager.r, cb, b[k], AU_STATE_HOVER);
        ca = a[k];
        cb = b[k];
        au_css_restyle(lazy.r, lazy.doc);
        au_css_restyle(eager.r, eager.doc);
        compare_docs(what, "hover", &lazy, &eager);
    }
    au_free(a);
    au_free(b);
    view_doc_free(&lazy);
    view_doc_free(&eager);
}

/* Every prefix of a sheet (a sheet cut off anywhere: inside a block, a
 * string, a comment, a selector): lazy and eager parses must style the
 * built-in page alike. */
static void lazy_truncations(const char *what, const char *css)
{
    size_t n = strlen(css);
    char *buf = malloc(n + 1);
    for (size_t k = 0; k <= n; k++) {
        memcpy(buf, css, k);
        buf[k] = 0;
        const char *extra[1] = {buf};
        css_lazy_bodies = true;
        ViewDoc lazy = view_doc(page, sizeof page - 1, extra, 1, 1024, 768);
        css_lazy_bodies = false;
        ViewDoc eager = view_doc(page, sizeof page - 1, extra, 1, 1024, 768);
        css_lazy_bodies = true;
        char step[48];
        snprintf(step, sizeof step, "cut at byte %u", (unsigned)k);
        compare_docs(what, step, &lazy, &eager);
        view_doc_free(&lazy);
        view_doc_free(&eager);
    }
    free(buf);
}

/* --lazy DIR: the sample pages (with wiki.css for the page that links
 * it), as written and with sheets of media queries and viewport units.
 * --lazy-page PAGE.html [SHEET.css ...]: one page with the sheets given. */
static int test_lazy(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--lazy-page") == 0) {
        size_t len;
        char *html = read_file(argv[2], &len);
        if (!html) {
            fprintf(stderr, "cannot read %s\n", argv[2]);
            return 2;
        }
        int ns = argc - 3;
        const char **css = calloc((size_t)ns + 1, sizeof *css);
        for (int i = 0; i < ns; i++) {
            size_t n;
            for (int k = 0; k < i && !css[i]; k++)
                if (!strcmp(argv[3 + k], argv[3 + i]))
                    css[i] = css[k]; /* a file given again is the same sheet */
            if (css[i])
                continue;
            css[i] = read_file(argv[3 + i], &n);
            if (!css[i]) {
                fprintf(stderr, "cannot read %s\n", argv[3 + i]);
                return 2;
            }
        }
        lazy_page(argv[2], html, len, css, ns);
        printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    const char *samples = argc > 2 ? argv[2] : NULL;
    static const char *const names[] = {"wiki.html",     "hn.html",       "hn-item.html", "blog.html",   "docs.html",
                                        "forum.html",    "reddit.html",   "images.html",  "svg.html",    "emoji.html",
                                        "qa-boxes.html", "qa-edge.html",  "qa-flex.html", "qa-forms.html",
                                        "qa-inline.html", "qa-quirks.html", "qa-tables.html", "webfonts.html",
                                        "grid.html",     "canvas.html"};
    for (size_t i = 0; samples && i < sizeof names / sizeof names[0]; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", samples, names[i]);
        size_t len, clen = 0;
        char *html = read_file(path, &len);
        CHECK(html != NULL);
        if (!html)
            continue;
        char *css = NULL;
        if (!strcmp(names[i], "wiki.html")) {
            snprintf(path, sizeof path, "%s/wiki.css", samples);
            css = read_file(path, &clen);
            CHECK(css != NULL);
        }
        const char *extra[4] = {sheet_viewport, sheet_units, sheet_lazy, css};
        lazy_page(names[i], html, len, extra + 3, css ? 1 : 0);
        lazy_page(names[i], html, len, extra, css ? 4 : 3);
        free(html);
        free(css);
    }
    lazy_page("built in", page, sizeof page - 1, (const char *const[]){sheet_plain, sheet_has_state, sheet_lazy}, 3);
    /* sheets given twice, with and without layers */
    lazy_page("repeated", page, sizeof page - 1,
              (const char *const[]){sheet_plain, sheet_lazy, sheet_has_state, sheet_plain, sheet_viewport, sheet_lazy,
                                    sheet_plain},
              7);
    lazy_page("repeated layers", page, sizeof page - 1,
              (const char *const[]){sheet_layers_between, sheet_layers, sheet_layers_between, sheet_plain,
                                    sheet_layers, sheet_layers_between},
              6);
    lazy_page("repeated layers first", page, sizeof page - 1,
              (const char *const[]){sheet_layers, sheet_layers_between, sheet_layers}, 3);
    lazy_truncations("cut sheet", sheet_lazy);
    lazy_truncations("cut layers", sheet_layers_between);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static bool in_body(const AuElement *el)
{
    for (const AuNode *p = &el->node; p; p = p->parent)
        if (p->type == AU_NODE_ELEMENT && ((const AuElement *)p)->tag == au_atom_cstr("body"))
            return true;
    return false;
}

static int bench(int argc, char **argv)
{
    bool verify = false;
    int a = 2;
    if (a < argc && strcmp(argv[a], "--verify") == 0) {
        verify = true;
        a++;
    }
    if (a >= argc) {
        fprintf(stderr, "usage: test_css_invalidate --bench [--verify] PAGE.html [SHEET.css ...]\n");
        return 2;
    }
    size_t len;
    char *html = read_file(argv[a], &len);
    if (!html) {
        fprintf(stderr, "cannot read %s\n", argv[a]);
        return 2;
    }
    AuDocument *doc = au_html_parse(html, len, NULL);
    free(html);
    AuCssResolver *r = au_css_resolver_new();
    uint32_t n;
    AuElement **el = elements(doc, &n);
    /* <style> elements, then the sheets given */
    for (uint32_t i = 0; i < n; i++) {
        if (el[i]->tag != au_atom_cstr("style"))
            continue;
        AuBuf t;
        au_buf_init(&t);
        for (AuNode *c = el[i]->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_TEXT)
                au_buf_append(&t, ((AuText *)c)->data, ((AuText *)c)->len);
        au_css_resolver_add_sheet(r, au_css_sheet_parse(t.data ? t.data : "", t.len, AU_CSS_ORIGIN_AUTHOR, NULL, NULL));
        au_buf_free(&t);
    }
    for (int i = a + 1; i < argc; i++) {
        char *css = read_file(argv[i], &len);
        if (!css) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        au_css_resolver_add_sheet(r, au_css_sheet_parse(css, len, AU_CSS_ORIGIN_AUTHOR, NULL, NULL));
        free(css);
    }
    double t0 = now_ms();
    au_css_style_document(r, doc);
    double full_ms = now_ms() - t0;
    /* hover targets: body elements, evenly spread */
    AuElement **tg = au_alloc((n ? n : 1) * sizeof *tg);
    uint32_t nt = 0;
    for (uint32_t i = 0; i < n; i++)
        if (in_body(el[i]) && el[i]->tag != au_atom_cstr("script") && el[i]->tag != au_atom_cstr("style"))
            tg[nt++] = el[i];
    uint32_t steps = nt < 300 ? nt : 300;
    printf("%s: %u elements, %u hover moves, :has() rules %s, full style %.2f ms\n", argv[a], (unsigned)n,
           (unsigned)steps, css_resolver_has_has(r) ? "yes" : "no", full_ms);
    for (int mode = 0; mode < 2 && steps; mode++) {
        AuElement *cur = NULL;
        double total = 0, worst = 0;
        uint32_t sum = 0, most = 0;
        /* the old way left ancestors that lost the hover unstyled */
        au_css_style_document(r, doc);
        for (uint32_t s = 0; s <= steps; s++) {
            AuElement *t = s == steps ? NULL : tg[(uint32_t)((uint64_t)s * nt / steps)];
            css_resolver_take_computed(r);
            double t1 = now_ms();
            if (mode == 0) {
                /* as before: only the two elements were reported */
                if (cur)
                    au_element_set_state(cur, AU_STATE_HOVER, false);
                if (t)
                    au_element_set_state(t, AU_STATE_HOVER, true);
                if (css_resolver_has_has(r)) {
                    au_css_style_document(r, doc);
                } else {
                    if (cur)
                        au_css_invalidate(r, cur, AU_CSS_CHANGE_ATTR, 0);
                    if (t)
                        au_css_invalidate(r, t, AU_CSS_CHANGE_ATTR, 0);
                    au_css_restyle(r, doc);
                }
            } else {
                au_css_move_state(r, cur, t, AU_STATE_HOVER);
                au_css_restyle(r, doc);
            }
            double dt = now_ms() - t1;
            uint32_t c = css_resolver_take_computed(r);
            cur = t;
            total += dt;
            if (dt > worst)
                worst = dt;
            sum += c;
            if (c > most)
                most = c;
            if (mode == 1 && c > 100 && t && getenv("INVALIDATE_DUMP")) {
                const AuAttr *cl = au_element_attr(t, AU_ATOM_class);
                fprintf(stderr, "  move %u to <%s class=\"%.60s\">: %u elements\n", (unsigned)s, au_atom_str(t->tag),
                        cl ? cl->value : "", (unsigned)c);
            }
            if (mode == 1 && verify && s < 40)
                restyle_and_compare(r, doc, "bench", (int)s);
        }
        printf("  %-8s elements restyled per move: mean %.1f, max %u; time per move: mean %.3f ms, max %.3f ms\n",
               mode == 0 ? "before:" : "targeted:", (double)sum / (steps + 1), (unsigned)most, total / (steps + 1),
               worst);
    }
    au_free(tg);
    au_free(el);
    au_css_clear_document(doc);
    au_node_unref(&doc->node);
    au_css_resolver_free(r);
    if (failures)
        printf("%d verification failures\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    init_props();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0)
        return bench(argc, argv);
    if (argc > 1 && (strcmp(argv[1], "--lazy") == 0 || strcmp(argv[1], "--lazy-page") == 0))
        return test_lazy(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--viewport") == 0)
        return test_viewport(argc > 2 ? argv[2] : NULL);
    uint32_t most = 0;
    run_page("plain", sheet_plain, NULL, &most);
    run_page("has-plain", sheet_plain, sheet_has_plain, &most);
    run_page("has-state", sheet_has_state, NULL, NULL);
    run_page("all", sheet_plain, sheet_has_state, NULL);
    run_page("all-has", sheet_has_plain, sheet_has_state, NULL);
    test_counts();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
