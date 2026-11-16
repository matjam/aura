/* HTML tree construction: HTML 13.2.6.
 *
 * Tokens arrive from the tokenizer through html_tb_emit. Character tokens
 * come as runs; each insertion mode consumes a run piece by piece with the
 * same result as one token per character.
 *
 * The stack of open elements and the list of active formatting elements
 * hold a reference on each element, so an element removed from the tree
 * (by the algorithm or, later, by script) stays valid while listed.
 *
 * Cost. Scope checks first consult a count of open HTML elements per tag
 * (tag_count), so the common "is there a p in button scope" query is O(1)
 * when no such element is open. Stack membership is a pointer hash set.
 * Noah's Ark and the "a" lookup skip the list scan unless enough elements
 * with the tag are listed. */
#include "html_internal.h"

#include <string.h>

/* ---- Tag flags ---- */

enum {
    F_SPECIAL = 1,
    F_FORMATTING = 2,
    F_SCOPE = 4,       /* bounds "in scope" (HTML namespace) */
    F_IMPLIED = 8,     /* generate implied end tags */
    F_THOROUGH = 16,   /* generate all implied end tags thoroughly */
    F_VOID = 32,       /* serializes as void */
    F_RAWTEXT = 64,    /* serializer writes text children literally */
    F_HEADING = 128
};

#define SP F_SPECIAL
static const uint16_t tag_flags[AU_ATOM_STATIC_COUNT] = {
    [AU_ATOM_address] = SP, [AU_ATOM_applet] = SP | F_SCOPE, [AU_ATOM_area] = SP | F_VOID,
    [AU_ATOM_article] = SP, [AU_ATOM_aside] = SP, [AU_ATOM_base] = SP | F_VOID,
    [AU_ATOM_basefont] = SP | F_VOID, [AU_ATOM_bgsound] = SP | F_VOID, [AU_ATOM_blockquote] = SP,
    [AU_ATOM_body] = SP, [AU_ATOM_br] = SP | F_VOID, [AU_ATOM_button] = SP,
    [AU_ATOM_caption] = SP | F_SCOPE | F_THOROUGH, [AU_ATOM_center] = SP, [AU_ATOM_col] = SP | F_VOID,
    [AU_ATOM_colgroup] = SP | F_THOROUGH, [AU_ATOM_dd] = SP | F_IMPLIED | F_THOROUGH,
    [AU_ATOM_details] = SP, [AU_ATOM_dir] = SP, [AU_ATOM_div] = SP, [AU_ATOM_dl] = SP,
    [AU_ATOM_dt] = SP | F_IMPLIED | F_THOROUGH, [AU_ATOM_embed] = SP | F_VOID, [AU_ATOM_fieldset] = SP,
    [AU_ATOM_figcaption] = SP, [AU_ATOM_figure] = SP, [AU_ATOM_footer] = SP, [AU_ATOM_form] = SP,
    [AU_ATOM_frame] = SP | F_VOID, [AU_ATOM_frameset] = SP,
    [AU_ATOM_h1] = SP | F_HEADING, [AU_ATOM_h2] = SP | F_HEADING, [AU_ATOM_h3] = SP | F_HEADING,
    [AU_ATOM_h4] = SP | F_HEADING, [AU_ATOM_h5] = SP | F_HEADING, [AU_ATOM_h6] = SP | F_HEADING,
    [AU_ATOM_head] = SP, [AU_ATOM_header] = SP, [AU_ATOM_hgroup] = SP, [AU_ATOM_hr] = SP | F_VOID,
    [AU_ATOM_html] = SP | F_SCOPE, [AU_ATOM_iframe] = SP | F_RAWTEXT, [AU_ATOM_img] = SP | F_VOID,
    [AU_ATOM_input] = SP | F_VOID, [AU_ATOM_keygen] = SP | F_VOID, [AU_ATOM_li] = SP | F_IMPLIED | F_THOROUGH,
    [AU_ATOM_link] = SP | F_VOID, [AU_ATOM_listing] = SP, [AU_ATOM_main] = SP,
    [AU_ATOM_marquee] = SP | F_SCOPE, [AU_ATOM_menu] = SP, [AU_ATOM_meta] = SP | F_VOID,
    [AU_ATOM_nav] = SP, [AU_ATOM_noembed] = SP | F_RAWTEXT, [AU_ATOM_noframes] = SP | F_RAWTEXT,
    [AU_ATOM_noscript] = SP, [AU_ATOM_object] = SP | F_SCOPE, [AU_ATOM_ol] = SP,
    [AU_ATOM_p] = SP | F_IMPLIED | F_THOROUGH, [AU_ATOM_param] = SP | F_VOID,
    [AU_ATOM_plaintext] = SP | F_RAWTEXT, [AU_ATOM_pre] = SP, [AU_ATOM_script] = SP | F_RAWTEXT,
    [AU_ATOM_search] = SP, [AU_ATOM_section] = SP, [AU_ATOM_select] = SP | F_SCOPE,
    [AU_ATOM_source] = SP | F_VOID, [AU_ATOM_style] = SP | F_RAWTEXT, [AU_ATOM_summary] = SP,
    [AU_ATOM_table] = SP | F_SCOPE, [AU_ATOM_tbody] = SP | F_THOROUGH,
    [AU_ATOM_td] = SP | F_SCOPE | F_THOROUGH, [AU_ATOM_template] = SP | F_SCOPE,
    [AU_ATOM_textarea] = SP, [AU_ATOM_tfoot] = SP | F_THOROUGH,
    [AU_ATOM_th] = SP | F_SCOPE | F_THOROUGH, [AU_ATOM_thead] = SP | F_THOROUGH, [AU_ATOM_title] = SP,
    [AU_ATOM_tr] = SP | F_THOROUGH, [AU_ATOM_track] = SP | F_VOID, [AU_ATOM_ul] = SP,
    [AU_ATOM_wbr] = SP | F_VOID, [AU_ATOM_xmp] = SP | F_RAWTEXT,
    [AU_ATOM_a] = F_FORMATTING, [AU_ATOM_b] = F_FORMATTING, [AU_ATOM_big] = F_FORMATTING,
    [AU_ATOM_code] = F_FORMATTING, [AU_ATOM_em] = F_FORMATTING, [AU_ATOM_font] = F_FORMATTING,
    [AU_ATOM_i] = F_FORMATTING, [AU_ATOM_nobr] = F_FORMATTING, [AU_ATOM_s] = F_FORMATTING,
    [AU_ATOM_small] = F_FORMATTING, [AU_ATOM_strike] = F_FORMATTING, [AU_ATOM_strong] = F_FORMATTING,
    [AU_ATOM_tt] = F_FORMATTING, [AU_ATOM_u] = F_FORMATTING,
    [AU_ATOM_optgroup] = F_IMPLIED | F_THOROUGH, [AU_ATOM_option] = F_IMPLIED | F_THOROUGH,
    [AU_ATOM_rb] = F_IMPLIED | F_THOROUGH, [AU_ATOM_rp] = F_IMPLIED | F_THOROUGH,
    [AU_ATOM_rt] = F_IMPLIED | F_THOROUGH, [AU_ATOM_rtc] = F_IMPLIED | F_THOROUGH,
};
#undef SP

static uint16_t flags_of(AuAtom tag)
{
    return tag < AU_ATOM_STATIC_COUNT ? tag_flags[tag] : 0;
}

bool html_serializes_void(AuAtom tag)
{
    return (flags_of(tag) & F_VOID) != 0;
}

bool html_serializes_rawtext(AuAtom tag)
{
    return (flags_of(tag) & F_RAWTEXT) != 0;
}

/* ---- Insertion modes ---- */

enum {
    M_INITIAL,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_IN_HEAD,
    M_IN_HEAD_NOSCRIPT,
    M_AFTER_HEAD,
    M_IN_BODY,
    M_TEXT,
    M_IN_TABLE,
    M_IN_TABLE_TEXT,
    M_IN_CAPTION,
    M_IN_COLUMN_GROUP,
    M_IN_TABLE_BODY,
    M_IN_ROW,
    M_IN_CELL,
    M_IN_TEMPLATE,
    M_AFTER_BODY,
    M_IN_FRAMESET,
    M_AFTER_FRAMESET,
    M_AFTER_AFTER_BODY,
    M_AFTER_AFTER_FRAMESET
};

/* An entry in the list of active formatting elements. el is NULL for a
 * marker and for the adoption agency's bookmark. The token's attributes
 * are kept so elements can be recreated and compared (Noah's Ark) as the
 * token had them. */
typedef struct FmtEntry {
    struct FmtEntry *prev, *next;   /* list order */
    struct FmtEntry *bprev, *bnext; /* hash bucket chain (elements only) */
    AuElement *el;
    AuAtom tag;
    AuAttr *attrs;
    uint32_t nattrs;
    uint32_t sig;   /* hash of tag and attributes */
    uint32_t depth; /* markers before this entry */
} FmtEntry;

/* An open-addressed map from pointer to pointer. */
typedef struct PtrMap {
    const void **keys;
    void **vals;
    uint32_t size, count;
} PtrMap;

struct HtmlTreeBuilder {
    AuDocument *doc;      /* the parser's Document */
    AuDocument *node_doc; /* document new nodes belong to */
    HtmlTokenizer *tok;

    AuElement **stack;
    uint32_t depth, stack_cap;
    AuElement **set;      /* hash set of the elements on the stack */
    uint32_t set_size, set_count;
    uint32_t *tag_count;  /* open HTML elements per tag atom */
    uint32_t tag_count_size;

    FmtEntry *afe_head, *afe_tail;
    uint32_t nafe;              /* element entries */
    uint32_t markers;           /* marker entries */
    FmtEntry **bhead, **btail;  /* Noah's Ark buckets */
    uint32_t nbuckets;
    PtrMap afe_map;             /* element to entry */
    uint32_t *afe_tag_count;    /* element entries per static tag atom */

    uint8_t *tmpl_modes;
    uint32_t ntmpl, tmpl_cap;

    uint32_t nselectedcontent; /* selectedcontent elements created */
    uint8_t mode, orig_mode;
    bool scripting, frameset_ok, foster, skip_lf, stopped;
    AuElement *head, *form, *context;
    AuBuf pending;        /* pending table character tokens */

    void (*script_hook)(void *ctx, AuElement *script);
    void *script_ctx;
};

static void dispatch(HtmlTreeBuilder *tb, HtmlToken *t);
static void process_html(HtmlTreeBuilder *tb, int mode, HtmlToken *t);
static void in_body(HtmlTreeBuilder *tb, HtmlToken *t);
static void in_head(HtmlTreeBuilder *tb, HtmlToken *t);
static void in_table(HtmlTreeBuilder *tb, HtmlToken *t);
static void chars_html(HtmlTreeBuilder *tb, const char *s, size_t n);

/* ---- Element predicates ---- */

static AuElement *E(AuNode *n)
{
    return (AuElement *)n;
}

static bool is_html(const AuElement *e, AuAtom tag)
{
    return e && e->ns == AU_NS_HTML && e->tag == tag;
}

static bool html_flag(const AuElement *e, uint16_t f)
{
    return e->ns == AU_NS_HTML && (flags_of(e->tag) & f);
}

static bool is_special(const AuElement *e)
{
    if (e->ns == AU_NS_HTML)
        return (flags_of(e->tag) & F_SPECIAL) != 0;
    if (e->ns == AU_NS_MATHML)
        return e->tag == AU_ATOM_mi || e->tag == AU_ATOM_mo || e->tag == AU_ATOM_mn || e->tag == AU_ATOM_ms ||
               e->tag == AU_ATOM_mtext || e->tag == AU_ATOM_annotation_xml;
    return e->tag == AU_ATOM_foreignObject || e->tag == AU_ATOM_desc || e->tag == AU_ATOM_title;
}

static bool is_mathml_text_ip(const AuElement *e)
{
    return e->ns == AU_NS_MATHML && (e->tag == AU_ATOM_mi || e->tag == AU_ATOM_mo || e->tag == AU_ATOM_mn ||
                                     e->tag == AU_ATOM_ms || e->tag == AU_ATOM_mtext);
}

static bool is_html_ip(const AuElement *e)
{
    if (e->ns == AU_NS_MATHML && e->tag == AU_ATOM_annotation_xml) {
        AuAttr *a = au_element_attr(e, AU_ATOM_encoding);
        return a && (au_ieq(a->value, a->len, "text/html", 9) ||
                     au_ieq(a->value, a->len, "application/xhtml+xml", 21));
    }
    return e->ns == AU_NS_SVG &&
           (e->tag == AU_ATOM_foreignObject || e->tag == AU_ATOM_desc || e->tag == AU_ATOM_title);
}

/* ---- Stack of open elements ---- */

static uint32_t ptr_slot(const void *p, uint32_t size)
{
    uintptr_t v = (uintptr_t)p;
    return (uint32_t)((v >> 3) * 2654435761u) & (size - 1);
}

static void set_add(HtmlTreeBuilder *tb, AuElement *e)
{
    if ((tb->set_count + 1) * 2 > tb->set_size) {
        uint32_t old = tb->set_size;
        AuElement **os = tb->set;
        tb->set_size = old ? old * 2 : 64;
        tb->set = au_calloc(tb->set_size * sizeof *tb->set);
        for (uint32_t i = 0; i < old; i++) {
            if (os[i]) {
                uint32_t j = ptr_slot(os[i], tb->set_size);
                while (tb->set[j])
                    j = (j + 1) & (tb->set_size - 1);
                tb->set[j] = os[i];
            }
        }
        au_free(os);
    }
    uint32_t j = ptr_slot(e, tb->set_size);
    while (tb->set[j])
        j = (j + 1) & (tb->set_size - 1);
    tb->set[j] = e;
    tb->set_count++;
}

static bool in_stack(const HtmlTreeBuilder *tb, const AuElement *e)
{
    if (!tb->set_size || !e)
        return false;
    uint32_t j = ptr_slot(e, tb->set_size);
    while (tb->set[j]) {
        if (tb->set[j] == e)
            return true;
        j = (j + 1) & (tb->set_size - 1);
    }
    return false;
}

/* Linear-probing deletion with backward shift. */
static void set_del(HtmlTreeBuilder *tb, AuElement *e)
{
    uint32_t mask = tb->set_size - 1;
    uint32_t j = ptr_slot(e, tb->set_size);
    while (tb->set[j] != e)
        j = (j + 1) & mask;
    tb->set[j] = NULL;
    tb->set_count--;
    uint32_t k = (j + 1) & mask;
    while (tb->set[k]) {
        uint32_t h = ptr_slot(tb->set[k], tb->set_size);
        /* Move k back to j if j lies cyclically between h and k. */
        if ((j < k) ? (h <= j || h > k) : (h <= j && h > k)) {
            tb->set[j] = tb->set[k];
            tb->set[k] = NULL;
            j = k;
        }
        k = (k + 1) & mask;
    }
}

/* tag_count is indexed by atom and grows to cover every tag pushed, so
 * "no such element is open" is O(1) for custom tags too. */
static void count_tag(HtmlTreeBuilder *tb, AuElement *e, int d)
{
    if (e->ns != AU_NS_HTML)
        return;
    if (e->tag >= tb->tag_count_size) {
        uint32_t size = tb->tag_count_size * 2;
        while (size <= e->tag)
            size *= 2;
        tb->tag_count = au_realloc(tb->tag_count, size * sizeof *tb->tag_count);
        memset(tb->tag_count + tb->tag_count_size, 0, (size - tb->tag_count_size) * sizeof *tb->tag_count);
        tb->tag_count_size = size;
    }
    tb->tag_count[e->tag] += (uint32_t)d;
}

static uint32_t open_count(const HtmlTreeBuilder *tb, AuAtom tag)
{
    return tag < tb->tag_count_size ? tb->tag_count[tag] : 0;
}

static void push(HtmlTreeBuilder *tb, AuElement *e)
{
    if (tb->depth == tb->stack_cap) {
        tb->stack_cap = tb->stack_cap ? tb->stack_cap * 2 : 32;
        tb->stack = au_realloc(tb->stack, tb->stack_cap * sizeof *tb->stack);
    }
    tb->stack[tb->depth++] = e;
    au_node_ref(&e->node);
    set_add(tb, e);
    count_tag(tb, e, 1);
}

/* ---- Finishing an element: option and selectedcontent ----
 * When an option's children are parsed and it is its select's selected
 * option, its contents are copied into the select's selectedcontent
 * elements (HTML 4.10.10, the option element's children parsed steps).
 * Selectedness here comes from the selected attributes, as no script or
 * user interaction has changed it during parsing. */

static AuElement *nearest_select(AuElement *e)
{
    bool optgroup = false;
    for (AuNode *n = e->node.parent; n && n->type == AU_NODE_ELEMENT; n = n->parent) {
        AuElement *a = E(n);
        if (a->ns != AU_NS_HTML)
            continue;
        if (a->tag == AU_ATOM_datalist || a->tag == AU_ATOM_hr || a->tag == AU_ATOM_option)
            return NULL;
        if (a->tag == AU_ATOM_optgroup) {
            if (optgroup)
                return NULL;
            optgroup = true;
        }
        if (a->tag == AU_ATOM_select)
            return a;
    }
    return NULL;
}

static bool option_disabled(const AuElement *o)
{
    if (au_element_attr(o, AU_ATOM_disabled))
        return true;
    AuNode *p = o->node.parent;
    return p && p->type == AU_NODE_ELEMENT && is_html(E(p), AU_ATOM_optgroup) &&
           au_element_attr(E(p), AU_ATOM_disabled);
}

/* The option the selectedness setting algorithm leaves selected. */
static AuElement *selected_option(AuElement *select)
{
    AuElement *first_enabled = NULL, *last_selected = NULL;
    AuNode *root = &select->node;
    AuNode *n = root->first_child;
    while (n) {
        bool skip = false;
        if (n->type == AU_NODE_ELEMENT && E(n)->ns == AU_NS_HTML) {
            AuElement *e = E(n);
            if (e->tag == AU_ATOM_option) {
                if (au_element_attr(e, AU_ATOM_selected))
                    last_selected = e;
                if (!first_enabled && !option_disabled(e))
                    first_enabled = e;
            }
            if (e->tag == AU_ATOM_select || e->tag == AU_ATOM_hr || e->tag == AU_ATOM_option ||
                e->tag == AU_ATOM_datalist)
                skip = true;
            if (e->tag == AU_ATOM_optgroup) {
                for (AuNode *a = n->parent; a && a != root; a = a->parent)
                    if (a->type == AU_NODE_ELEMENT && is_html(E(a), AU_ATOM_optgroup))
                        skip = true;
            }
        }
        n = skip ? au_node_next_skip(n, root) : au_node_next(n, root);
    }
    if (last_selected)
        return last_selected;
    const char *size = au_element_get_attr(select, AU_ATOM_size);
    if (size) {
        while (au_is_space((unsigned char)*size))
            size++;
        if (au_is_digit((unsigned char)*size)) {
            unsigned long v = 0;
            while (au_is_digit((unsigned char)*size) && v < 1000000)
                v = v * 10 + (unsigned long)(*size++ - '0');
            if (v != 1)
                return NULL;
        }
    }
    return first_enabled;
}

static AuNode *clone_node(AuDocument *doc, const AuNode *n)
{
    switch (n->type) {
    case AU_NODE_ELEMENT: {
        const AuElement *s = (const AuElement *)n;
        AuElement *e = E(au_element_new(doc, s->tag, (AuNamespace)s->ns));
        for (uint32_t i = 0; i < s->nattrs; i++)
            au_element_append_attr(e, s->attrs[i].name, (AuAttrNamespace)s->attrs[i].ns, s->attrs[i].value,
                                   s->attrs[i].len);
        if (s->template_content)
            e->template_content = au_fragment_new(doc);
        return &e->node;
    }
    case AU_NODE_TEXT:
        return au_text_new(doc, ((const AuText *)n)->data, ((const AuText *)n)->len);
    case AU_NODE_COMMENT:
        return au_comment_new(doc, ((const AuText *)n)->data, ((const AuText *)n)->len);
    case AU_NODE_PROCESSING_INSTRUCTION: {
        const AuProcessingInstruction *pi = (const AuProcessingInstruction *)n;
        return au_pi_new(doc, pi->target, pi->text.data, pi->text.len);
    }
    default:
        return NULL;
    }
}

static bool clone_dropped(const AuNode *n)
{
    if (n->type != AU_NODE_ELEMENT)
        return false;
    const AuElement *e = (const AuElement *)n;
    if (e->ns == AU_NS_SVG)
        return e->tag == AU_ATOM_script;
    return e->ns == AU_NS_HTML && (e->tag == AU_ATOM_script || e->tag == AU_ATOM_frame || e->tag == AU_ATOM_iframe ||
                                   e->tag == AU_ATOM_embed || e->tag == AU_ATOM_object);
}

/* Replaces dst's children with deep copies of src's children, leaving out
 * script, frame, iframe, embed and object elements. Iterative. */
static void clone_children(AuDocument *doc, const AuNode *src, AuNode *dst)
{
    while (dst->first_child)
        au_node_remove(dst->first_child);
    const AuNode *n = src->first_child;
    AuNode *parent = dst;
    while (n) {
        AuNode *c = clone_dropped(n) ? NULL : clone_node(doc, n);
        if (c) {
            au_node_append(parent, c);
            au_node_unref(c);
        }
        if (c && n->first_child) {
            parent = c;
            n = n->first_child;
            continue;
        }
        while (n && n != src && !n->next) {
            n = n->parent;
            if (n != src)
                parent = parent->parent;
        }
        if (!n || n == src)
            break;
        n = n->next;
    }
}

static bool selectedcontent_enabled(const AuElement *sc, const AuElement *select)
{
    for (AuNode *a = sc->node.parent; a && a != &select->node; a = a->parent) {
        if (a->type != AU_NODE_ELEMENT || E(a)->ns != AU_NS_HTML)
            continue;
        AuAtom t = E(a)->tag;
        if (t == AU_ATOM_select || t == AU_ATOM_option || t == AU_ATOM_selectedcontent)
            return false;
    }
    return true;
}

static void option_children_parsed(HtmlTreeBuilder *tb, AuElement *option)
{
    AuElement *select = nearest_select(option);
    if (!select || au_element_attr(select, AU_ATOM_multiple) || selected_option(select) != option)
        return;
    AuNode *root = &select->node;
    AuNode **list = NULL;
    size_t count = 0, cap = 0;
    for (AuNode *n = root->first_child; n; n = au_node_next(n, root)) {
        if (n->type == AU_NODE_ELEMENT && is_html(E(n), AU_ATOM_selectedcontent) &&
            selectedcontent_enabled(E(n), select)) {
            if (count == cap) {
                cap = cap ? cap * 2 : 4;
                list = au_realloc(list, cap * sizeof *list);
            }
            list[count++] = n;
        }
    }
    for (size_t i = 0; i < count; i++)
        clone_children(tb->node_doc, &option->node, list[i]);
    au_free(list);
}

static void stack_remove_at(HtmlTreeBuilder *tb, uint32_t i)
{
    AuElement *e = tb->stack[i];
    if (tb->nselectedcontent && is_html(e, AU_ATOM_option))
        option_children_parsed(tb, e);
    memmove(tb->stack + i, tb->stack + i + 1, (tb->depth - i - 1) * sizeof *tb->stack);
    tb->depth--;
    set_del(tb, e);
    count_tag(tb, e, -1);
    au_node_unref(&e->node);
}

static void pop(HtmlTreeBuilder *tb)
{
    stack_remove_at(tb, tb->depth - 1);
}

static int stack_index(const HtmlTreeBuilder *tb, const AuElement *e)
{
    if (!in_stack(tb, e))
        return -1;
    for (uint32_t i = tb->depth; i-- > 0;)
        if (tb->stack[i] == e)
            return (int)i;
    return -1;
}

static void stack_remove(HtmlTreeBuilder *tb, AuElement *e)
{
    int i = stack_index(tb, e);
    if (i >= 0)
        stack_remove_at(tb, (uint32_t)i);
}

static void stack_insert_at(HtmlTreeBuilder *tb, uint32_t i, AuElement *e)
{
    push(tb, e); /* grows, refs and records it */
    memmove(tb->stack + i + 1, tb->stack + i, (tb->depth - 1 - i) * sizeof *tb->stack);
    tb->stack[i] = e;
}

static AuElement *cur(const HtmlTreeBuilder *tb)
{
    return tb->depth ? tb->stack[tb->depth - 1] : NULL;
}

static AuElement *adjusted_cur(const HtmlTreeBuilder *tb)
{
    if (tb->context && tb->depth == 1)
        return tb->context;
    return cur(tb);
}

static bool template_open(const HtmlTreeBuilder *tb)
{
    return tb->tag_count[AU_ATOM_template] > 0;
}

/* "The parser is parsing template contents." */
static bool parsing_template(const HtmlTreeBuilder *tb)
{
    return template_open(tb) || is_html(tb->context, AU_ATOM_template);
}

enum { SCOPE_DEFAULT, SCOPE_LIST, SCOPE_BUTTON, SCOPE_TABLE };

static bool scope_boundary(const AuElement *e, int scope)
{
    if (scope == SCOPE_TABLE)
        return e->ns == AU_NS_HTML && (e->tag == AU_ATOM_html || e->tag == AU_ATOM_table || e->tag == AU_ATOM_template);
    if (e->ns == AU_NS_HTML) {
        if (flags_of(e->tag) & F_SCOPE)
            return true;
        if (scope == SCOPE_LIST)
            return e->tag == AU_ATOM_ol || e->tag == AU_ATOM_ul;
        if (scope == SCOPE_BUTTON)
            return e->tag == AU_ATOM_button;
        return false;
    }
    if (e->ns == AU_NS_MATHML)
        return is_mathml_text_ip(e) || e->tag == AU_ATOM_annotation_xml;
    return e->tag == AU_ATOM_foreignObject || e->tag == AU_ATOM_desc || e->tag == AU_ATOM_title;
}

/* True if an HTML element with tag is in the given scope. */
static bool in_scope(const HtmlTreeBuilder *tb, AuAtom tag, int scope)
{
    if (!open_count(tb, tag))
        return false;
    for (uint32_t i = tb->depth; i-- > 0;) {
        AuElement *e = tb->stack[i];
        if (is_html(e, tag))
            return true;
        if (scope_boundary(e, scope))
            return false;
    }
    return false;
}

static bool element_in_scope(const HtmlTreeBuilder *tb, const AuElement *target)
{
    if (!in_stack(tb, target))
        return false;
    for (uint32_t i = tb->depth; i-- > 0;) {
        AuElement *e = tb->stack[i];
        if (e == target)
            return true;
        if (scope_boundary(e, SCOPE_DEFAULT))
            return false;
    }
    return false;
}

static bool heading_in_scope(const HtmlTreeBuilder *tb)
{
    for (uint32_t i = tb->depth; i-- > 0;) {
        AuElement *e = tb->stack[i];
        if (html_flag(e, F_HEADING))
            return true;
        if (scope_boundary(e, SCOPE_DEFAULT))
            return false;
    }
    return false;
}

static bool table_section_in_table_scope(const HtmlTreeBuilder *tb)
{
    return in_scope(tb, AU_ATOM_tbody, SCOPE_TABLE) || in_scope(tb, AU_ATOM_thead, SCOPE_TABLE) ||
           in_scope(tb, AU_ATOM_tfoot, SCOPE_TABLE);
}

static void pop_until_tag(HtmlTreeBuilder *tb, AuAtom tag)
{
    while (tb->depth) {
        bool hit = is_html(cur(tb), tag);
        pop(tb);
        if (hit)
            break;
    }
}

static void pop_until_el(HtmlTreeBuilder *tb, AuElement *e)
{
    while (tb->depth) {
        bool hit = cur(tb) == e;
        pop(tb);
        if (hit)
            break;
    }
}

static void implied_end_tags(HtmlTreeBuilder *tb, AuAtom except)
{
    while (tb->depth) {
        AuElement *c = cur(tb);
        if (!html_flag(c, F_IMPLIED) || c->tag == except)
            break;
        pop(tb);
    }
}

static void implied_end_tags_thorough(HtmlTreeBuilder *tb)
{
    while (tb->depth && html_flag(cur(tb), F_THOROUGH))
        pop(tb);
}

static void close_p(HtmlTreeBuilder *tb)
{
    implied_end_tags(tb, AU_ATOM_p);
    pop_until_tag(tb, AU_ATOM_p);
}

static void close_p_if_in_button_scope(HtmlTreeBuilder *tb)
{
    if (in_scope(tb, AU_ATOM_p, SCOPE_BUTTON))
        close_p(tb);
}

/* ---- Creating and inserting nodes ---- */

typedef struct {
    AuNode *parent, *before;
} Place;

static AuNode *template_target(AuElement *e)
{
    if (is_html(e, AU_ATOM_template) && e->template_content)
        return e->template_content;
    return &e->node;
}

static Place appropriate_place(HtmlTreeBuilder *tb, AuElement *override)
{
    Place p = {NULL, NULL};
    AuElement *target = override ? override : cur(tb);
    if (tb->foster && target->ns == AU_NS_HTML &&
        (target->tag == AU_ATOM_table || target->tag == AU_ATOM_tbody || target->tag == AU_ATOM_tfoot ||
         target->tag == AU_ATOM_thead || target->tag == AU_ATOM_tr)) {
        int last = -1;
        for (uint32_t i = tb->depth; i-- > 0;) {
            if (is_html(tb->stack[i], AU_ATOM_template) || is_html(tb->stack[i], AU_ATOM_table)) {
                last = (int)i;
                break;
            }
        }
        if (last < 0) {
            p.parent = &tb->stack[0]->node;
            return p;
        }
        AuElement *lt = tb->stack[last];
        if (is_html(lt, AU_ATOM_template)) {
            target = lt;
        } else if (lt->node.parent) {
            p.parent = lt->node.parent;
            p.before = &lt->node;
            if (p.parent->type == AU_NODE_ELEMENT && is_html(E(p.parent), AU_ATOM_template)) {
                p.parent = template_target(E(p.parent));
                p.before = NULL;
            }
            return p;
        } else {
            target = tb->stack[last - 1];
        }
    }
    p.parent = template_target(target);
    return p;
}

static void insert_at(Place p, AuNode *n)
{
    if (p.parent->type == AU_NODE_DOCUMENT && n->type == AU_NODE_ELEMENT &&
        au_document_element((AuDocument *)p.parent))
        return;
    au_node_insert_before(p.parent, n, p.before);
}

static AuElement *create_element(HtmlTreeBuilder *tb, AuAtom tag, AuNamespace ns, const HtmlToken *t)
{
    AuElement *e = E(au_element_new(tb->node_doc, tag, ns));
    if (ns == AU_NS_HTML && tag == AU_ATOM_template)
        e->template_content = au_fragment_new(tb->node_doc);
    if (ns == AU_NS_HTML && tag == AU_ATOM_selectedcontent)
        tb->nselectedcontent++;
    if (t) {
        for (uint32_t i = 0; i < t->nattrs; i++) {
            const HtmlAttr *a = &t->attrs[i];
            au_element_append_attr(e, a->name, (AuAttrNamespace)a->ns, t->vals + a->voff, a->vlen);
        }
    }
    return e;
}

/* Inserts e at the appropriate place and pushes it. Drops the creation
 * reference: the tree and the stack own the element now. */
static AuElement *insert_and_push(HtmlTreeBuilder *tb, AuElement *e)
{
    insert_at(appropriate_place(tb, NULL), &e->node);
    push(tb, e);
    au_node_unref(&e->node);
    return e;
}

static AuElement *insert_foreign(HtmlTreeBuilder *tb, const HtmlToken *t, AuNamespace ns)
{
    return insert_and_push(tb, create_element(tb, t->name, ns, t));
}

static AuElement *insert_html(HtmlTreeBuilder *tb, const HtmlToken *t)
{
    return insert_foreign(tb, t, AU_NS_HTML);
}

/* Inserts an HTML element for a start tag with no attributes. */
static AuElement *insert_html_tag(HtmlTreeBuilder *tb, AuAtom tag)
{
    return insert_and_push(tb, create_element(tb, tag, AU_NS_HTML, NULL));
}

static void insert_text(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    if (!n)
        return;
    Place p = appropriate_place(tb, NULL);
    if (p.parent->type == AU_NODE_DOCUMENT)
        return;
    AuNode *prev = p.before ? p.before->prev : p.parent->last_child;
    if (prev && prev->type == AU_NODE_TEXT) {
        au_text_append((AuText *)prev, s, n);
        return;
    }
    AuNode *text = au_text_new(tb->node_doc, s, n);
    au_node_insert_before(p.parent, text, p.before);
    au_node_unref(text);
}

static void insert_comment_at(HtmlTreeBuilder *tb, const HtmlToken *t, AuNode *parent)
{
    Place p = parent ? (Place){parent, NULL} : appropriate_place(tb, NULL);
    AuNode *c = au_comment_new(tb->node_doc, t->data, t->len);
    au_node_insert_before(p.parent, c, p.before);
    au_node_unref(c);
}

static void insert_pi_at(HtmlTreeBuilder *tb, const HtmlToken *t, AuNode *parent)
{
    Place p = parent ? (Place){parent, NULL} : appropriate_place(tb, NULL);
    AuNode *c = au_pi_new(tb->node_doc, t->name, t->data, t->len);
    au_node_insert_before(p.parent, c, p.before);
    au_node_unref(c);
}

static void insert_comment(HtmlTreeBuilder *tb, const HtmlToken *t)
{
    insert_comment_at(tb, t, NULL);
}

static void insert_pi(HtmlTreeBuilder *tb, const HtmlToken *t)
{
    insert_pi_at(tb, t, NULL);
}

/* ---- Foreign content adjustments ---- */

typedef struct {
    const char *from, *to;
} NamePair;

static const NamePair svg_attrs[] = {
    {"attributename", "attributeName"}, {"attributetype", "attributeType"},
    {"basefrequency", "baseFrequency"}, {"baseprofile", "baseProfile"},
    {"calcmode", "calcMode"}, {"clippathunits", "clipPathUnits"},
    {"diffuseconstant", "diffuseConstant"}, {"edgemode", "edgeMode"},
    {"filterunits", "filterUnits"}, {"glyphref", "glyphRef"},
    {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"},
    {"keypoints", "keyPoints"}, {"keysplines", "keySplines"}, {"keytimes", "keyTimes"},
    {"lengthadjust", "lengthAdjust"}, {"limitingconeangle", "limitingConeAngle"},
    {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"}, {"maskcontentunits", "maskContentUnits"},
    {"maskunits", "maskUnits"}, {"numoctaves", "numOctaves"}, {"pathlength", "pathLength"},
    {"patterncontentunits", "patternContentUnits"}, {"patterntransform", "patternTransform"},
    {"patternunits", "patternUnits"}, {"pointsatx", "pointsAtX"}, {"pointsaty", "pointsAtY"},
    {"pointsatz", "pointsAtZ"}, {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"}, {"primitiveunits", "primitiveUnits"},
    {"refx", "refX"}, {"refy", "refY"}, {"repeatcount", "repeatCount"}, {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"}, {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"}, {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"}, {"startoffset", "startOffset"},
    {"stddeviation", "stdDeviation"}, {"stitchtiles", "stitchTiles"},
    {"surfacescale", "surfaceScale"}, {"systemlanguage", "systemLanguage"},
    {"tablevalues", "tableValues"}, {"targetx", "targetX"}, {"targety", "targetY"},
    {"textlength", "textLength"}, {"viewbox", "viewBox"}, {"viewtarget", "viewTarget"},
    {"xchannelselector", "xChannelSelector"}, {"ychannelselector", "yChannelSelector"},
    {"zoomandpan", "zoomAndPan"},
};

static const NamePair svg_tags[] = {
    {"altglyph", "altGlyph"}, {"altglyphdef", "altGlyphDef"}, {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"}, {"animatemotion", "animateMotion"},
    {"animatetransform", "animateTransform"}, {"clippath", "clipPath"}, {"feblend", "feBlend"},
    {"fecolormatrix", "feColorMatrix"}, {"fecomponenttransfer", "feComponentTransfer"},
    {"fecomposite", "feComposite"}, {"feconvolvematrix", "feConvolveMatrix"},
    {"fediffuselighting", "feDiffuseLighting"}, {"fedisplacementmap", "feDisplacementMap"},
    {"fedistantlight", "feDistantLight"}, {"fedropshadow", "feDropShadow"}, {"feflood", "feFlood"},
    {"fefunca", "feFuncA"}, {"fefuncb", "feFuncB"}, {"fefuncg", "feFuncG"}, {"fefuncr", "feFuncR"},
    {"fegaussianblur", "feGaussianBlur"}, {"feimage", "feImage"}, {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"}, {"femorphology", "feMorphology"}, {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"}, {"fespecularlighting", "feSpecularLighting"},
    {"fespotlight", "feSpotLight"}, {"fetile", "feTile"}, {"feturbulence", "feTurbulence"},
    {"foreignobject", "foreignObject"}, {"glyphref", "glyphRef"},
    {"lineargradient", "linearGradient"}, {"radialgradient", "radialGradient"},
    {"textpath", "textPath"},
};

static const char *lookup_pair(const NamePair *tab, size_t n, const char *s)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(s, tab[mid].from);
        if (!c)
            return tab[mid].to;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static void adjust_svg_attrs(HtmlToken *t)
{
    for (uint32_t i = 0; i < t->nattrs; i++) {
        const char *to = lookup_pair(svg_attrs, sizeof svg_attrs / sizeof *svg_attrs, au_atom_str(t->attrs[i].name));
        if (to)
            t->attrs[i].name = au_atom_cstr(to);
    }
}

static void adjust_mathml_attrs(HtmlToken *t)
{
    static AuAtom lower;
    if (!lower)
        lower = au_atom_cstr("definitionurl");
    for (uint32_t i = 0; i < t->nattrs; i++)
        if (t->attrs[i].name == lower)
            t->attrs[i].name = AU_ATOM_definitionURL;
}

static void adjust_foreign_attrs(HtmlToken *t)
{
    for (uint32_t i = 0; i < t->nattrs; i++) {
        const char *s = au_atom_str(t->attrs[i].name);
        if (!strncmp(s, "xlink:", 6)) {
            const char *l = s + 6;
            if (!strcmp(l, "actuate") || !strcmp(l, "arcrole") || !strcmp(l, "href") || !strcmp(l, "role") ||
                !strcmp(l, "show") || !strcmp(l, "title") || !strcmp(l, "type"))
                t->attrs[i].ns = AU_ATTR_NS_XLINK;
        } else if (!strcmp(s, "xml:lang") || !strcmp(s, "xml:space")) {
            t->attrs[i].ns = AU_ATTR_NS_XML;
        } else if (!strcmp(s, "xmlns") || !strcmp(s, "xmlns:xlink")) {
            t->attrs[i].ns = AU_ATTR_NS_XMLNS;
        }
    }
}

static AuAtom adjust_svg_tag(AuAtom tag)
{
    const char *to = lookup_pair(svg_tags, sizeof svg_tags / sizeof *svg_tags, au_atom_str(tag));
    return to ? au_atom_cstr(to) : tag;
}

/* ---- Active formatting elements ----
 * A doubly linked list of heap entries, so removal and insertion anywhere
 * are O(1); a map from element to entry; and hash buckets keyed by
 * (signature of tag and attributes, marker depth). An entry's marker depth
 * is the number of markers before it, so entries "after the last marker"
 * are those whose depth equals the current marker count. Within a bucket,
 * entries with the same key stay in list order, which lets Noah's Ark find
 * its matches without scanning the list. */

#define BOOKMARK_DEPTH 0xFFFFFFFFu /* depth of the adoption agency's bookmark */

static uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}

static void *map_get(const PtrMap *m, const void *k)
{
    if (!m->size)
        return NULL;
    uint32_t i = ptr_slot(k, m->size);
    while (m->keys[i]) {
        if (m->keys[i] == k)
            return m->vals[i];
        i = (i + 1) & (m->size - 1);
    }
    return NULL;
}

static void map_put(PtrMap *m, const void *k, void *v)
{
    if ((m->count + 1) * 2 > m->size) {
        uint32_t old = m->size;
        const void **ok = m->keys;
        void **ov = m->vals;
        m->size = old ? old * 2 : 64;
        m->keys = au_calloc(m->size * sizeof *m->keys);
        m->vals = au_calloc(m->size * sizeof *m->vals);
        m->count = 0;
        for (uint32_t i = 0; i < old; i++)
            if (ok[i])
                map_put(m, ok[i], ov[i]);
        au_free((void *)ok);
        au_free(ov);
    }
    uint32_t i = ptr_slot(k, m->size);
    while (m->keys[i])
        i = (i + 1) & (m->size - 1);
    m->keys[i] = k;
    m->vals[i] = v;
    m->count++;
}

static void map_del(PtrMap *m, const void *k)
{
    uint32_t mask = m->size - 1;
    uint32_t j = ptr_slot(k, m->size);
    while (m->keys[j] != k)
        j = (j + 1) & mask;
    m->keys[j] = NULL;
    m->count--;
    for (uint32_t i = (j + 1) & mask; m->keys[i]; i = (i + 1) & mask) {
        uint32_t h = ptr_slot(m->keys[i], m->size);
        if ((j < i) ? (h <= j || h > i) : (h <= j && h > i)) {
            m->keys[j] = m->keys[i];
            m->vals[j] = m->vals[i];
            m->keys[i] = NULL;
            j = i;
        }
    }
}

static uint32_t entry_sig(AuAtom tag, const AuAttr *a, uint32_t n)
{
    uint32_t h = mix(tag * 2654435761u);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = a[i].name * 0x9E3779B1u;
        for (uint32_t k = 0; k < a[i].len; k++)
            x = (x ^ (unsigned char)a[i].value[k]) * 16777619u;
        h += mix(x); /* a sum, so attribute order does not matter */
    }
    return h;
}

static uint32_t bucket_of(const HtmlTreeBuilder *tb, const FmtEntry *e)
{
    return mix(e->sig + e->depth * 0x9E3779B9u) & (tb->nbuckets - 1);
}

static void bucket_link_after(HtmlTreeBuilder *tb, FmtEntry *e, FmtEntry *after)
{
    uint32_t b = bucket_of(tb, e);
    e->bprev = after;
    e->bnext = after ? after->bnext : tb->bhead[b];
    if (after)
        after->bnext = e;
    if (e->bnext)
        e->bnext->bprev = e;
    else
        tb->btail[b] = e;
    if (!e->bprev)
        tb->bhead[b] = e;
}

static void bucket_unlink(HtmlTreeBuilder *tb, FmtEntry *e)
{
    uint32_t b = bucket_of(tb, e);
    if (e->bprev)
        e->bprev->bnext = e->bnext;
    else
        tb->bhead[b] = e->bnext;
    if (e->bnext)
        e->bnext->bprev = e->bprev;
    else
        tb->btail[b] = e->bprev;
    e->bprev = e->bnext = NULL;
}

static void buckets_grow(HtmlTreeBuilder *tb)
{
    if (tb->nafe < tb->nbuckets)
        return;
    tb->nbuckets = tb->nbuckets ? tb->nbuckets * 2 : 16;
    au_free(tb->bhead);
    au_free(tb->btail);
    tb->bhead = au_calloc(tb->nbuckets * sizeof *tb->bhead);
    tb->btail = au_calloc(tb->nbuckets * sizeof *tb->btail);
    for (FmtEntry *e = tb->afe_head; e; e = e->next)
        if (e->el)
            bucket_link_after(tb, e, tb->btail[bucket_of(tb, e)]);
}

static void list_link_after(HtmlTreeBuilder *tb, FmtEntry *e, FmtEntry *after)
{
    e->prev = after;
    e->next = after ? after->next : tb->afe_head;
    if (after)
        after->next = e;
    else
        tb->afe_head = e;
    if (e->next)
        e->next->prev = e;
    else
        tb->afe_tail = e;
}

static void list_unlink(HtmlTreeBuilder *tb, FmtEntry *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        tb->afe_head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        tb->afe_tail = e->prev;
    e->prev = e->next = NULL;
}

static void afe_tag_count_add(HtmlTreeBuilder *tb, AuAtom tag, int d)
{
    if (tag < AU_ATOM_STATIC_COUNT)
        tb->afe_tag_count[tag] += (uint32_t)d;
}

/* Adds element entry e (el, tag, attrs, sig and depth set) after `after`
 * in the list and after `bafter` in its bucket. */
static void afe_add(HtmlTreeBuilder *tb, FmtEntry *e, FmtEntry *after, FmtEntry *bafter)
{
    tb->nafe++;
    buckets_grow(tb);
    list_link_after(tb, e, after);
    bucket_link_after(tb, e, bafter ? bafter : tb->btail[bucket_of(tb, e)]);
    map_put(&tb->afe_map, e->el, e);
    afe_tag_count_add(tb, e->tag, 1);
    au_node_ref(&e->el->node);
}

/* Removes and frees an entry: an element, a marker or the bookmark. */
static void afe_free(HtmlTreeBuilder *tb, FmtEntry *e)
{
    if (e->el) {
        bucket_unlink(tb, e);
        map_del(&tb->afe_map, e->el);
        afe_tag_count_add(tb, e->tag, -1);
        tb->nafe--;
        au_node_unref(&e->el->node);
    } else if (e->depth != BOOKMARK_DEPTH) {
        tb->markers--;
    }
    list_unlink(tb, e);
    for (uint32_t i = 0; i < e->nattrs; i++)
        au_free(e->attrs[i].value);
    au_free(e->attrs);
    au_free(e);
}

static void afe_push_marker(HtmlTreeBuilder *tb)
{
    FmtEntry *e = au_calloc(sizeof *e);
    e->depth = tb->markers++;
    list_link_after(tb, e, tb->afe_tail);
}

static FmtEntry *afe_find(const HtmlTreeBuilder *tb, const AuElement *el)
{
    return map_get(&tb->afe_map, el);
}

/* The last element entry with tag after the last marker, or NULL. */
static FmtEntry *afe_last_with_tag(const HtmlTreeBuilder *tb, AuAtom tag)
{
    if (tag < AU_ATOM_STATIC_COUNT && !tb->afe_tag_count[tag])
        return NULL;
    for (FmtEntry *e = tb->afe_tail; e && e->el; e = e->prev)
        if (e->tag == tag)
            return e;
    return NULL;
}

static bool same_attrs(const FmtEntry *a, const FmtEntry *b)
{
    if (a->nattrs != b->nattrs)
        return false;
    for (uint32_t i = 0; i < a->nattrs; i++) {
        uint32_t j;
        for (j = 0; j < b->nattrs; j++)
            if (b->attrs[j].name == a->attrs[i].name)
                break;
        if (j == b->nattrs || b->attrs[j].len != a->attrs[i].len ||
            memcmp(b->attrs[j].value, a->attrs[i].value, a->attrs[i].len))
            return false;
    }
    return true;
}

static void afe_push(HtmlTreeBuilder *tb, AuElement *el, const HtmlToken *t)
{
    FmtEntry *f = au_calloc(sizeof *f);
    f->el = el;
    f->tag = el->tag;
    f->nattrs = t->nattrs;
    f->attrs = t->nattrs ? au_alloc(t->nattrs * sizeof *f->attrs) : NULL;
    for (uint32_t i = 0; i < t->nattrs; i++) {
        f->attrs[i].name = t->attrs[i].name;
        f->attrs[i].ns = 0;
        f->attrs[i].value = au_strndup(t->vals + t->attrs[i].voff, t->attrs[i].vlen);
        f->attrs[i].len = t->attrs[i].vlen;
    }
    f->sig = entry_sig(f->tag, f->attrs, f->nattrs);
    f->depth = tb->markers;
    /* Noah's Ark: at most three matching entries after the last marker.
     * Same-key entries are in list order in the bucket, so the third
     * match found from the tail is the earliest. */
    if ((f->tag >= AU_ATOM_STATIC_COUNT || tb->afe_tag_count[f->tag] >= 3) && tb->nbuckets) {
        int matches = 0;
        for (FmtEntry *g = tb->btail[bucket_of(tb, f)]; g; g = g->bprev) {
            if (g->depth == f->depth && g->sig == f->sig && g->tag == f->tag && same_attrs(g, f) &&
                ++matches == 3) {
                afe_free(tb, g);
                break;
            }
        }
    }
    afe_add(tb, f, tb->afe_tail, NULL);
}

static AuElement *create_from_entry(HtmlTreeBuilder *tb, const FmtEntry *f)
{
    AuElement *e = E(au_element_new(tb->node_doc, f->tag, AU_NS_HTML));
    for (uint32_t i = 0; i < f->nattrs; i++)
        au_element_append_attr(e, f->attrs[i].name, AU_ATTR_NS_NONE, f->attrs[i].value, f->attrs[i].len);
    return e;
}

static void afe_replace_el(HtmlTreeBuilder *tb, FmtEntry *f, AuElement *e)
{
    map_del(&tb->afe_map, f->el);
    au_node_ref(&e->node);
    au_node_unref(&f->el->node);
    f->el = e;
    map_put(&tb->afe_map, e, f);
}

/* Moves entry f to just after the bookmark bm, keeping f's bucket in list
 * order. bm may lie before or after f; both directions are walked at once,
 * so the cost is proportional to the distance between them. */
static void afe_move_to_bookmark(HtmlTreeBuilder *tb, FmtEntry *f, FmtEntry *bm)
{
    if (f->next == bm)
        return;
    bool forward = false;
    for (FmtEntry *a = f->next, *b = bm->next;; a = a ? a->next : NULL, b = b ? b->next : NULL) {
        if (a == bm) {
            forward = true;
            break;
        }
        if (b == f)
            break;
    }
    uint32_t bk = bucket_of(tb, f);
    FmtEntry *same = NULL;
    if (forward) {
        /* f moves later: it goes after the last same-bucket entry it passes. */
        for (FmtEntry *e = f->next; e != bm; e = e->next)
            if (e->el && bucket_of(tb, e) == bk)
                same = e;
    } else {
        /* f moves earlier: it goes before the first same-bucket entry it passes. */
        for (FmtEntry *e = bm->next; e != f && !same; e = e->next)
            if (e->el && bucket_of(tb, e) == bk)
                same = e;
    }
    list_unlink(tb, f);
    list_link_after(tb, f, bm);
    if (same) {
        bucket_unlink(tb, f);
        bucket_link_after(tb, f, forward ? same : same->bprev);
    }
}

static void reconstruct_afe(HtmlTreeBuilder *tb)
{
    FmtEntry *e = tb->afe_tail;
    if (!e || !e->el || in_stack(tb, e->el))
        return;
    while (e->prev && e->prev->el && !in_stack(tb, e->prev->el))
        e = e->prev;
    for (; e; e = e->next) {
        AuElement *el = create_from_entry(tb, e);
        afe_replace_el(tb, e, el);
        insert_and_push(tb, el);
    }
}

static void clear_afe_to_marker(HtmlTreeBuilder *tb)
{
    while (tb->afe_tail) {
        bool marker = !tb->afe_tail->el;
        afe_free(tb, tb->afe_tail);
        if (marker)
            break;
    }
}

/* ---- Insertion mode helpers ---- */

static void reset_mode(HtmlTreeBuilder *tb)
{
    bool last = false;
    for (uint32_t i = tb->depth; i-- > 0;) {
        AuElement *node = tb->stack[i];
        if (i == 0) {
            last = true;
            if (tb->context)
                node = tb->context;
        }
        if (node->ns == AU_NS_HTML) {
            AuAtom t = node->tag;
            if ((t == AU_ATOM_td || t == AU_ATOM_th) && !last) {
                tb->mode = M_IN_CELL;
                return;
            }
            if (t == AU_ATOM_tr) {
                tb->mode = M_IN_ROW;
                return;
            }
            if (t == AU_ATOM_tbody || t == AU_ATOM_thead || t == AU_ATOM_tfoot) {
                tb->mode = M_IN_TABLE_BODY;
                return;
            }
            if (t == AU_ATOM_caption) {
                tb->mode = M_IN_CAPTION;
                return;
            }
            if (t == AU_ATOM_colgroup) {
                tb->mode = M_IN_COLUMN_GROUP;
                return;
            }
            if (t == AU_ATOM_table) {
                tb->mode = M_IN_TABLE;
                return;
            }
            if (t == AU_ATOM_template) {
                tb->mode = tb->tmpl_modes[tb->ntmpl - 1];
                return;
            }
            if (t == AU_ATOM_head && !last) {
                tb->mode = M_IN_HEAD;
                return;
            }
            if (t == AU_ATOM_body) {
                tb->mode = M_IN_BODY;
                return;
            }
            if (t == AU_ATOM_frameset) {
                tb->mode = M_IN_FRAMESET;
                return;
            }
            if (t == AU_ATOM_html) {
                tb->mode = tb->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
                return;
            }
        }
        if (last) {
            tb->mode = M_IN_BODY;
            return;
        }
    }
}

static void push_tmpl_mode(HtmlTreeBuilder *tb, uint8_t m)
{
    if (tb->ntmpl == tb->tmpl_cap) {
        tb->tmpl_cap = tb->tmpl_cap ? tb->tmpl_cap * 2 : 8;
        tb->tmpl_modes = au_realloc(tb->tmpl_modes, tb->tmpl_cap);
    }
    tb->tmpl_modes[tb->ntmpl++] = m;
}

static void set_pointer(AuElement **slot, AuElement *e)
{
    if (e)
        au_node_ref(&e->node);
    if (*slot)
        au_node_unref(&(*slot)->node);
    *slot = e;
}

static void generic_text(HtmlTreeBuilder *tb, HtmlToken *t, int state)
{
    insert_html(tb, t);
    html_tok_set_state(tb->tok, state);
    tb->orig_mode = tb->mode;
    tb->mode = M_TEXT;
}

static bool is_ws(int c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static size_t ws_prefix(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && is_ws((unsigned char)s[i]))
        i++;
    return i;
}

static bool has_attr_ci(const HtmlToken *t, AuAtom name, const char *val)
{
    for (uint32_t i = 0; i < t->nattrs; i++)
        if (t->attrs[i].name == name)
            return au_ieq(t->vals + t->attrs[i].voff, t->attrs[i].vlen, val, strlen(val));
    return false;
}

static bool token_has_attr(const HtmlToken *t, AuAtom name)
{
    for (uint32_t i = 0; i < t->nattrs; i++)
        if (t->attrs[i].name == name)
            return true;
    return false;
}

static void merge_attrs(AuElement *e, const HtmlToken *t)
{
    for (uint32_t i = 0; i < t->nattrs; i++)
        if (!au_element_attr(e, t->attrs[i].name))
            au_element_append_attr(e, t->attrs[i].name, AU_ATTR_NS_NONE, t->vals + t->attrs[i].voff,
                                   t->attrs[i].vlen);
}

static void run_script_hook(HtmlTreeBuilder *tb, AuElement *script)
{
    if (tb->script_hook) {
        au_node_ref(&script->node);
        tb->script_hook(tb->script_ctx, script);
        au_node_unref(&script->node);
    }
}

/* Stops parsing: "the end" pops every element off the stack, which
 * finishes parsing their children. */
static void stop(HtmlTreeBuilder *tb)
{
    tb->stopped = true;
    while (tb->depth)
        pop(tb);
}

/* ---- The adoption agency algorithm ----
 * Returns true if the caller should act as for "any other end tag". */
static bool adoption_agency(HtmlTreeBuilder *tb, AuAtom subject)
{
    AuElement *c = cur(tb);
    if (is_html(c, subject) && !afe_find(tb, c)) {
        pop(tb);
        return false;
    }
    for (int outer = 0; outer < 8; outer++) {
        FmtEntry *fent = afe_last_with_tag(tb, subject);
        if (!fent)
            return true;
        AuElement *fe = fent->el;
        int fe_idx = stack_index(tb, fe);
        if (fe_idx < 0) {
            afe_free(tb, fent);
            return false;
        }
        if (!element_in_scope(tb, fe))
            return false;
        int fb_idx = -1;
        for (uint32_t i = (uint32_t)fe_idx + 1; i < tb->depth; i++) {
            if (is_special(tb->stack[i])) {
                fb_idx = (int)i;
                break;
            }
        }
        if (fb_idx < 0) {
            pop_until_el(tb, fe);
            afe_free(tb, fent);
            return false;
        }
        AuElement *furthest = tb->stack[fb_idx];
        AuElement *common = tb->stack[fe_idx - 1];
        FmtEntry *bm = au_calloc(sizeof *bm); /* the bookmark */
        bm->depth = BOOKMARK_DEPTH;
        list_link_after(tb, bm, fent);
        AuElement *last_node = furthest;
        au_node_ref(&last_node->node);
        int node_idx = fb_idx;
        for (int inner = 1;; inner++) {
            node_idx--;
            AuElement *node = tb->stack[node_idx];
            if (node == fe)
                break;
            FmtEntry *ae = afe_find(tb, node);
            if (inner > 3 && ae) {
                afe_free(tb, ae);
                ae = NULL;
            }
            if (!ae) {
                stack_remove_at(tb, (uint32_t)node_idx);
                fb_idx--;
                continue;
            }
            AuElement *ne = create_from_entry(tb, ae);
            afe_replace_el(tb, ae, ne);
            /* Replace node with ne on the stack. */
            set_del(tb, node);
            count_tag(tb, node, -1);
            au_node_unref(&node->node);
            tb->stack[node_idx] = ne;
            au_node_ref(&ne->node);
            set_add(tb, ne);
            count_tag(tb, ne, 1);
            au_node_unref(&ne->node); /* creation reference; the stack and list hold it */
            node = ne;
            if (last_node == furthest) {
                list_unlink(tb, bm);
                list_link_after(tb, bm, ae);
            }
            au_node_append(&node->node, &last_node->node);
            au_node_ref(&node->node);
            au_node_unref(&last_node->node);
            last_node = node;
        }
        Place p = appropriate_place(tb, common);
        insert_at(p, &last_node->node);
        au_node_unref(&last_node->node);

        AuElement *ne = create_from_entry(tb, fent);
        au_node_reparent_children(&furthest->node, &ne->node);
        au_node_append(&furthest->node, &ne->node);

        afe_move_to_bookmark(tb, fent, bm);
        afe_replace_el(tb, fent, ne);
        afe_free(tb, bm);

        stack_remove(tb, fe);
        fb_idx = stack_index(tb, furthest);
        stack_insert_at(tb, (uint32_t)fb_idx + 1, ne);
        au_node_unref(&ne->node); /* creation reference */
    }
    return false;
}

static void any_other_end_tag(HtmlTreeBuilder *tb, AuAtom tag)
{
    if (!open_count(tb, tag))
        return;
    for (uint32_t i = tb->depth; i-- > 0;) {
        AuElement *node = tb->stack[i];
        if (is_html(node, tag)) {
            implied_end_tags(tb, tag);
            pop_until_el(tb, node);
            return;
        }
        if (is_special(node))
            return;
    }
}

/* ---- Character runs ---- */

static void body_chars(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    while (n) {
        if (*s == 0) { /* parse error; ignore */
            s++;
            n--;
            continue;
        }
        size_t k = 0;
        bool nonws = false;
        while (k < n && s[k]) {
            if (!is_ws((unsigned char)s[k]))
                nonws = true;
            k++;
        }
        reconstruct_afe(tb);
        insert_text(tb, s, k);
        if (nonws)
            tb->frameset_ok = false;
        s += k;
        n -= k;
    }
}

/* Inserts only the whitespace characters of a run (the others are parse
 * errors and ignored). */
static void ws_only_chars(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t k = i;
        while (k < n && is_ws((unsigned char)s[k]))
            k++;
        insert_text(tb, s + i, k - i);
        while (k < n && !is_ws((unsigned char)s[k]))
            k++;
        i = k;
    }
}

static void flush_table_text(HtmlTreeBuilder *tb)
{
    const char *s = tb->pending.data;
    size_t n = tb->pending.len;
    bool nonws = false;
    for (size_t i = 0; i < n; i++)
        if (!is_ws((unsigned char)s[i]))
            nonws = true;
    if (nonws) {
        /* Parse error: process with "in table" anything-else rules. */
        bool old = tb->foster;
        tb->foster = true;
        body_chars(tb, s, n);
        tb->foster = old;
    } else {
        insert_text(tb, s, n);
    }
    tb->pending.len = 0;
}

static void foreign_chars(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t k = i;
        while (k < n && s[k]) {
            if (!is_ws((unsigned char)s[k]))
                tb->frameset_ok = false;
            k++;
        }
        insert_text(tb, s + i, k - i);
        if (k < n) {
            insert_text(tb, "\xEF\xBF\xBD", 3);
            k++;
        }
        i = k;
    }
}

static void chars_html(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    while (n) {
        size_t w;
        switch (tb->mode) {
        case M_INITIAL:
            w = ws_prefix(s, n);
            s += w;
            n -= w;
            if (!n)
                return;
            tb->doc->quirks = AU_QUIRKS_FULL;
            tb->mode = M_BEFORE_HTML;
            continue;
        case M_BEFORE_HTML:
            w = ws_prefix(s, n);
            s += w;
            n -= w;
            if (!n)
                return;
            {
                AuElement *e = create_element(tb, AU_ATOM_html, AU_NS_HTML, NULL);
                au_node_append(&tb->doc->node, &e->node);
                push(tb, e);
                au_node_unref(&e->node);
            }
            tb->mode = M_BEFORE_HEAD;
            continue;
        case M_BEFORE_HEAD:
            w = ws_prefix(s, n);
            s += w;
            n -= w;
            if (!n)
                return;
            set_pointer(&tb->head, insert_html_tag(tb, AU_ATOM_head));
            tb->mode = M_IN_HEAD;
            continue;
        case M_IN_HEAD:
        case M_IN_HEAD_NOSCRIPT:
        case M_AFTER_HEAD:
            w = ws_prefix(s, n);
            insert_text(tb, s, w);
            s += w;
            n -= w;
            if (!n)
                return;
            if (tb->mode == M_IN_HEAD) {
                pop(tb);
                tb->mode = M_AFTER_HEAD;
            } else if (tb->mode == M_IN_HEAD_NOSCRIPT) {
                pop(tb);
                tb->mode = M_IN_HEAD;
            } else {
                insert_html_tag(tb, AU_ATOM_body);
                tb->frameset_ok = true;
                tb->mode = M_IN_BODY;
            }
            continue;
        case M_IN_BODY:
        case M_IN_CAPTION:
        case M_IN_CELL:
        case M_IN_TEMPLATE:
            body_chars(tb, s, n);
            return;
        case M_TEXT:
            insert_text(tb, s, n);
            return;
        case M_IN_TABLE:
        case M_IN_TABLE_BODY:
        case M_IN_ROW: {
            AuElement *c = cur(tb);
            if (c->ns == AU_NS_HTML &&
                (c->tag == AU_ATOM_table || c->tag == AU_ATOM_tbody || c->tag == AU_ATOM_template ||
                 c->tag == AU_ATOM_tfoot || c->tag == AU_ATOM_thead || c->tag == AU_ATOM_tr)) {
                tb->pending.len = 0;
                tb->orig_mode = tb->mode;
                tb->mode = M_IN_TABLE_TEXT;
                continue;
            }
            bool old = tb->foster;
            tb->foster = true;
            body_chars(tb, s, n);
            tb->foster = old;
            return;
        }
        case M_IN_TABLE_TEXT:
            for (size_t i = 0; i < n; i++)
                if (s[i])
                    au_buf_putc(&tb->pending, s[i]);
            return;
        case M_IN_COLUMN_GROUP:
            w = ws_prefix(s, n);
            insert_text(tb, s, w);
            s += w;
            n -= w;
            if (!n)
                return;
            if (!is_html(cur(tb), AU_ATOM_colgroup)) {
                ws_only_chars(tb, s, n);
                return;
            }
            pop(tb);
            tb->mode = M_IN_TABLE;
            continue;
        case M_IN_FRAMESET:
        case M_AFTER_FRAMESET:
            ws_only_chars(tb, s, n);
            return;
        case M_AFTER_BODY:
        case M_AFTER_AFTER_BODY:
            w = ws_prefix(s, n);
            body_chars(tb, s, w);
            s += w;
            n -= w;
            if (!n)
                return;
            tb->mode = M_IN_BODY;
            continue;
        case M_AFTER_AFTER_FRAMESET:
            w = ws_prefix(s, n);
            body_chars(tb, s, w);
            return;
        default:
            return;
        }
    }
}

static void process_chars(HtmlTreeBuilder *tb, const char *s, size_t n)
{
    if (tb->stopped)
        return;
    if (tb->skip_lf) {
        tb->skip_lf = false;
        if (n && *s == '\n') {
            s++;
            n--;
        }
    }
    if (!n)
        return;
    AuElement *a = adjusted_cur(tb);
    if (!a || a->ns == AU_NS_HTML || is_mathml_text_ip(a) || is_html_ip(a))
        chars_html(tb, s, n);
    else
        foreign_chars(tb, s, n);
}

/* ---- Insertion modes for non-character tokens ---- */

#define IS_START(t, tag) ((t)->type == HTML_TOK_START && (t)->name == (tag))
#define IS_END(t, tag) ((t)->type == HTML_TOK_END && (t)->name == (tag))

static bool one_of(AuAtom a, const AuAtom *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (list[i] == a)
            return true;
    return false;
}
#define ONE_OF(a, ...) one_of((a), (const AuAtom[]){__VA_ARGS__}, sizeof((const AuAtom[]){__VA_ARGS__}) / sizeof(AuAtom))

static void initial(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (t->type == HTML_TOK_COMMENT) {
        insert_comment_at(tb, t, &tb->doc->node);
        return;
    }
    if (t->type == HTML_TOK_PI) {
        insert_pi_at(tb, t, &tb->doc->node);
        return;
    }
    if (t->type == HTML_TOK_DOCTYPE) {
        char *name = au_strndup(t->data, t->len);
        char *pub = au_strndup(t->pub, t->publen);
        char *sys = au_strndup(t->sys, t->syslen);
        AuNode *dt = au_doctype_new(tb->node_doc, name, pub, sys);
        bool has = false;
        for (AuNode *c = tb->doc->node.first_child; c; c = c->next)
            if (c->type == AU_NODE_DOCTYPE || c->type == AU_NODE_ELEMENT)
                has = true;
        if (!has)
            au_node_append(&tb->doc->node, dt);
        au_node_unref(dt);

        static const char *const quirk_prefixes[] = {
            "+//silmaril//dtd html pro v0r11 19970101//",
            "-//as//dtd html 3.0 aswedit + extensions//",
            "-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
            "-//ietf//dtd html 2.0 level 1//",
            "-//ietf//dtd html 2.0 level 2//",
            "-//ietf//dtd html 2.0 strict level 1//",
            "-//ietf//dtd html 2.0 strict level 2//",
            "-//ietf//dtd html 2.0 strict//",
            "-//ietf//dtd html 2.0//",
            "-//ietf//dtd html 2.1e//",
            "-//ietf//dtd html 3.0//",
            "-//ietf//dtd html 3.2 final//",
            "-//ietf//dtd html 3.2//",
            "-//ietf//dtd html 3//",
            "-//ietf//dtd html level 0//",
            "-//ietf//dtd html level 1//",
            "-//ietf//dtd html level 2//",
            "-//ietf//dtd html level 3//",
            "-//ietf//dtd html strict level 0//",
            "-//ietf//dtd html strict level 1//",
            "-//ietf//dtd html strict level 2//",
            "-//ietf//dtd html strict level 3//",
            "-//ietf//dtd html strict//",
            "-//ietf//dtd html//",
            "-//metrius//dtd metrius presentational//",
            "-//microsoft//dtd internet explorer 2.0 html strict//",
            "-//microsoft//dtd internet explorer 2.0 html//",
            "-//microsoft//dtd internet explorer 2.0 tables//",
            "-//microsoft//dtd internet explorer 3.0 html strict//",
            "-//microsoft//dtd internet explorer 3.0 html//",
            "-//microsoft//dtd internet explorer 3.0 tables//",
            "-//netscape comm. corp.//dtd html//",
            "-//netscape comm. corp.//dtd strict html//",
            "-//o'reilly and associates//dtd html 2.0//",
            "-//o'reilly and associates//dtd html extended 1.0//",
            "-//o'reilly and associates//dtd html extended relaxed 1.0//",
            "-//sq//dtd html 2.0 hotmetal + extensions//",
            "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
            "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
            "-//spyglass//dtd html 2.0 extended//",
            "-//sun microsystems corp.//dtd hotjava html//",
            "-//sun microsystems corp.//dtd hotjava strict html//",
            "-//w3c//dtd html 3 1995-03-24//",
            "-//w3c//dtd html 3.2 draft//",
            "-//w3c//dtd html 3.2 final//",
            "-//w3c//dtd html 3.2//",
            "-//w3c//dtd html 3.2s draft//",
            "-//w3c//dtd html 4.0 frameset//",
            "-//w3c//dtd html 4.0 transitional//",
            "-//w3c//dtd html experimental 19960712//",
            "-//w3c//dtd html experimental 970421//",
            "-//w3c//dtd w3 html//",
            "-//w3o//dtd w3 html 3.0//",
            "-//webtechs//dtd mozilla html 2.0//",
            "-//webtechs//dtd mozilla html//",
        };
#define PREFIX(s) (t->publen >= strlen(s) && au_ieq(pub, strlen(s), s, strlen(s)))
#define EQ(v, vl, s) au_ieq(v, vl, s, strlen(s))
        bool quirks = t->force_quirks || !(t->len == 4 && !memcmp(name, "html", 4));
        if (!quirks && t->has_public) {
            if (EQ(pub, t->publen, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
                EQ(pub, t->publen, "-/W3C/DTD HTML 4.0 Transitional/EN") || EQ(pub, t->publen, "HTML"))
                quirks = true;
            for (size_t i = 0; !quirks && i < sizeof quirk_prefixes / sizeof *quirk_prefixes; i++)
                if (PREFIX(quirk_prefixes[i]))
                    quirks = true;
            if (!quirks && !t->has_system &&
                (PREFIX("-//w3c//dtd html 4.01 frameset//") || PREFIX("-//w3c//dtd html 4.01 transitional//")))
                quirks = true;
        }
        if (!quirks && t->has_system &&
            EQ(sys, t->syslen, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
            quirks = true;
        if (quirks) {
            tb->doc->quirks = AU_QUIRKS_FULL;
        } else if (t->has_public &&
                   (PREFIX("-//w3c//dtd xhtml 1.0 frameset//") || PREFIX("-//w3c//dtd xhtml 1.0 transitional//") ||
                    (t->has_system && (PREFIX("-//w3c//dtd html 4.01 frameset//") ||
                                       PREFIX("-//w3c//dtd html 4.01 transitional//"))))) {
            tb->doc->quirks = AU_QUIRKS_LIMITED;
        }
#undef PREFIX
#undef EQ
        au_free(name);
        au_free(pub);
        au_free(sys);
        tb->mode = M_BEFORE_HTML;
        return;
    }
    tb->doc->quirks = AU_QUIRKS_FULL;
    tb->mode = M_BEFORE_HTML;
    dispatch(tb, t);
}

static void before_html(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_COMMENT:
        insert_comment_at(tb, t, &tb->doc->node);
        return;
    case HTML_TOK_PI:
        insert_pi_at(tb, t, &tb->doc->node);
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            AuElement *e = create_element(tb, AU_ATOM_html, AU_NS_HTML, t);
            insert_at((Place){&tb->doc->node, NULL}, &e->node);
            push(tb, e);
            au_node_unref(&e->node);
            tb->mode = M_BEFORE_HEAD;
            return;
        }
        break;
    case HTML_TOK_END:
        if (!ONE_OF(t->name, AU_ATOM_head, AU_ATOM_body, AU_ATOM_html, AU_ATOM_br))
            return;
        break;
    }
    AuElement *e = create_element(tb, AU_ATOM_html, AU_NS_HTML, NULL);
    insert_at((Place){&tb->doc->node, NULL}, &e->node);
    push(tb, e);
    au_node_unref(&e->node);
    tb->mode = M_BEFORE_HEAD;
    dispatch(tb, t);
}

static void before_head(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (t->name == AU_ATOM_head) {
            set_pointer(&tb->head, insert_html(tb, t));
            tb->mode = M_IN_HEAD;
            return;
        }
        break;
    case HTML_TOK_END:
        if (!ONE_OF(t->name, AU_ATOM_head, AU_ATOM_body, AU_ATOM_html, AU_ATOM_br))
            return;
        break;
    }
    set_pointer(&tb->head, insert_html_tag(tb, AU_ATOM_head));
    tb->mode = M_IN_HEAD;
    dispatch(tb, t);
}

static void end_template(HtmlTreeBuilder *tb)
{
    if (!template_open(tb))
        return;
    implied_end_tags_thorough(tb);
    pop_until_tag(tb, AU_ATOM_template);
    clear_afe_to_marker(tb);
    tb->ntmpl--;
    reset_mode(tb);
}

static void in_head(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START: {
        AuAtom n = t->name;
        if (n == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (ONE_OF(n, AU_ATOM_base, AU_ATOM_basefont, AU_ATOM_bgsound, AU_ATOM_link, AU_ATOM_meta)) {
            /* A <meta charset> encoding change would be handled here. */
            insert_html(tb, t);
            pop(tb);
            return;
        }
        if (n == AU_ATOM_title) {
            generic_text(tb, t, HTML_STATE_RCDATA);
            return;
        }
        if ((n == AU_ATOM_noscript && tb->scripting) || n == AU_ATOM_noframes || n == AU_ATOM_style) {
            generic_text(tb, t, HTML_STATE_RAWTEXT);
            return;
        }
        if (n == AU_ATOM_noscript) {
            insert_html(tb, t);
            tb->mode = M_IN_HEAD_NOSCRIPT;
            return;
        }
        if (n == AU_ATOM_script) {
            insert_html(tb, t);
            html_tok_set_state(tb->tok, HTML_STATE_SCRIPT_DATA);
            tb->orig_mode = tb->mode;
            tb->mode = M_TEXT;
            return;
        }
        if (n == AU_ATOM_template) {
            afe_push_marker(tb);
            tb->frameset_ok = false;
            tb->mode = M_IN_TEMPLATE;
            push_tmpl_mode(tb, M_IN_TEMPLATE);
            /* Declarative shadow roots are not enabled: a plain template. */
            insert_html(tb, t);
            return;
        }
        if (n == AU_ATOM_head)
            return;
        break;
    }
    case HTML_TOK_END:
        if (t->name == AU_ATOM_head) {
            pop(tb);
            tb->mode = M_AFTER_HEAD;
            return;
        }
        if (t->name == AU_ATOM_template) {
            end_template(tb);
            return;
        }
        if (!ONE_OF(t->name, AU_ATOM_body, AU_ATOM_html, AU_ATOM_br))
            return;
        break;
    }
    pop(tb);
    tb->mode = M_AFTER_HEAD;
    dispatch(tb, t);
}

static void in_head_noscript(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_COMMENT:
    case HTML_TOK_PI:
        in_head(tb, t);
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (ONE_OF(t->name, AU_ATOM_basefont, AU_ATOM_bgsound, AU_ATOM_link, AU_ATOM_meta, AU_ATOM_noframes,
                   AU_ATOM_style)) {
            in_head(tb, t);
            return;
        }
        if (t->name == AU_ATOM_head || t->name == AU_ATOM_noscript)
            return;
        break;
    case HTML_TOK_END:
        if (t->name == AU_ATOM_noscript) {
            pop(tb);
            tb->mode = M_IN_HEAD;
            return;
        }
        if (t->name != AU_ATOM_br)
            return;
        break;
    }
    pop(tb);
    tb->mode = M_IN_HEAD;
    dispatch(tb, t);
}

static void after_head(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START: {
        AuAtom n = t->name;
        if (n == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (n == AU_ATOM_body) {
            insert_html(tb, t);
            tb->frameset_ok = false;
            tb->mode = M_IN_BODY;
            return;
        }
        if (n == AU_ATOM_frameset) {
            insert_html(tb, t);
            tb->mode = M_IN_FRAMESET;
            return;
        }
        if (ONE_OF(n, AU_ATOM_base, AU_ATOM_basefont, AU_ATOM_bgsound, AU_ATOM_link, AU_ATOM_meta,
                   AU_ATOM_noframes, AU_ATOM_script, AU_ATOM_style, AU_ATOM_template, AU_ATOM_title)) {
            AuElement *head = tb->head;
            au_node_ref(&head->node);
            push(tb, head);
            in_head(tb, t);
            stack_remove(tb, head);
            au_node_unref(&head->node);
            return;
        }
        if (n == AU_ATOM_head)
            return;
        break;
    }
    case HTML_TOK_END:
        if (t->name == AU_ATOM_template) {
            in_head(tb, t);
            return;
        }
        if (!ONE_OF(t->name, AU_ATOM_body, AU_ATOM_html, AU_ATOM_br))
            return;
        break;
    }
    insert_html_tag(tb, AU_ATOM_body);
    tb->frameset_ok = true;
    tb->mode = M_IN_BODY;
    dispatch(tb, t);
}

static void in_body_start(HtmlTreeBuilder *tb, HtmlToken *t)
{
    AuAtom n = t->name;
    switch (n) {
    case AU_ATOM_html:
        if (!template_open(tb))
            merge_attrs(tb->stack[0], t);
        return;
    case AU_ATOM_base: case AU_ATOM_basefont: case AU_ATOM_bgsound: case AU_ATOM_link: case AU_ATOM_meta:
    case AU_ATOM_noframes: case AU_ATOM_script: case AU_ATOM_style: case AU_ATOM_template: case AU_ATOM_title:
        in_head(tb, t);
        return;
    case AU_ATOM_body:
        if (tb->depth == 1 || !is_html(tb->stack[1], AU_ATOM_body) || template_open(tb))
            return;
        tb->frameset_ok = false;
        merge_attrs(tb->stack[1], t);
        return;
    case AU_ATOM_frameset:
        if (tb->depth == 1 || !is_html(tb->stack[1], AU_ATOM_body))
            return;
        if (!tb->frameset_ok)
            return;
        if (tb->stack[1]->node.parent)
            au_node_remove(&tb->stack[1]->node);
        while (tb->depth > 1)
            pop(tb);
        insert_html(tb, t);
        tb->mode = M_IN_FRAMESET;
        return;
    case AU_ATOM_address: case AU_ATOM_article: case AU_ATOM_aside: case AU_ATOM_blockquote:
    case AU_ATOM_center: case AU_ATOM_details: case AU_ATOM_dialog: case AU_ATOM_dir: case AU_ATOM_div:
    case AU_ATOM_dl: case AU_ATOM_fieldset: case AU_ATOM_figcaption: case AU_ATOM_figure:
    case AU_ATOM_footer: case AU_ATOM_header: case AU_ATOM_hgroup: case AU_ATOM_main: case AU_ATOM_menu:
    case AU_ATOM_nav: case AU_ATOM_ol: case AU_ATOM_p: case AU_ATOM_search: case AU_ATOM_section:
    case AU_ATOM_summary: case AU_ATOM_ul:
        close_p_if_in_button_scope(tb);
        insert_html(tb, t);
        return;
    case AU_ATOM_h1: case AU_ATOM_h2: case AU_ATOM_h3: case AU_ATOM_h4: case AU_ATOM_h5: case AU_ATOM_h6:
        close_p_if_in_button_scope(tb);
        if (html_flag(cur(tb), F_HEADING))
            pop(tb);
        insert_html(tb, t);
        return;
    case AU_ATOM_pre: case AU_ATOM_listing:
        close_p_if_in_button_scope(tb);
        insert_html(tb, t);
        tb->skip_lf = true;
        tb->frameset_ok = false;
        return;
    case AU_ATOM_form:
        if (tb->form && !parsing_template(tb))
            return;
        close_p_if_in_button_scope(tb);
        {
            AuElement *e = insert_html(tb, t);
            if (!parsing_template(tb))
                set_pointer(&tb->form, e);
        }
        return;
    case AU_ATOM_li:
    case AU_ATOM_dd:
    case AU_ATOM_dt:
        tb->frameset_ok = false;
        if (n == AU_ATOM_li ? tb->tag_count[AU_ATOM_li] > 0
                            : (tb->tag_count[AU_ATOM_dd] > 0 || tb->tag_count[AU_ATOM_dt] > 0)) {
            for (uint32_t i = tb->depth; i-- > 0;) {
                AuElement *node = tb->stack[i];
                AuAtom hit = 0;
                if (n == AU_ATOM_li && is_html(node, AU_ATOM_li))
                    hit = AU_ATOM_li;
                else if (n != AU_ATOM_li && (is_html(node, AU_ATOM_dd) || is_html(node, AU_ATOM_dt)))
                    hit = node->tag;
                if (hit) {
                    implied_end_tags(tb, hit);
                    pop_until_tag(tb, hit);
                    break;
                }
                if (is_special(node) && !is_html(node, AU_ATOM_address) && !is_html(node, AU_ATOM_div) &&
                    !is_html(node, AU_ATOM_p))
                    break;
            }
        }
        close_p_if_in_button_scope(tb);
        insert_html(tb, t);
        return;
    case AU_ATOM_plaintext:
        close_p_if_in_button_scope(tb);
        insert_html(tb, t);
        html_tok_set_state(tb->tok, HTML_STATE_PLAINTEXT);
        return;
    case AU_ATOM_button:
        if (in_scope(tb, AU_ATOM_button, SCOPE_DEFAULT)) {
            implied_end_tags(tb, 0);
            pop_until_tag(tb, AU_ATOM_button);
        }
        reconstruct_afe(tb);
        insert_html(tb, t);
        tb->frameset_ok = false;
        return;
    case AU_ATOM_a: {
        FmtEntry *ae = afe_last_with_tag(tb, AU_ATOM_a);
        if (ae) {
            AuElement *e = ae->el;
            au_node_ref(&e->node);
            if (adoption_agency(tb, AU_ATOM_a))
                any_other_end_tag(tb, AU_ATOM_a);
            ae = afe_find(tb, e);
            if (ae)
                afe_free(tb, ae);
            stack_remove(tb, e);
            au_node_unref(&e->node);
        }
        reconstruct_afe(tb);
        afe_push(tb, insert_html(tb, t), t);
        return;
    }
    case AU_ATOM_b: case AU_ATOM_big: case AU_ATOM_code: case AU_ATOM_em: case AU_ATOM_font: case AU_ATOM_i:
    case AU_ATOM_s: case AU_ATOM_small: case AU_ATOM_strike: case AU_ATOM_strong: case AU_ATOM_tt:
    case AU_ATOM_u:
        reconstruct_afe(tb);
        afe_push(tb, insert_html(tb, t), t);
        return;
    case AU_ATOM_nobr:
        reconstruct_afe(tb);
        if (in_scope(tb, AU_ATOM_nobr, SCOPE_DEFAULT)) {
            if (adoption_agency(tb, AU_ATOM_nobr))
                any_other_end_tag(tb, AU_ATOM_nobr);
            reconstruct_afe(tb);
        }
        afe_push(tb, insert_html(tb, t), t);
        return;
    case AU_ATOM_applet: case AU_ATOM_marquee: case AU_ATOM_object:
        reconstruct_afe(tb);
        insert_html(tb, t);
        afe_push_marker(tb);
        tb->frameset_ok = false;
        return;
    case AU_ATOM_table:
        if (tb->doc->quirks != AU_QUIRKS_FULL)
            close_p_if_in_button_scope(tb);
        insert_html(tb, t);
        tb->frameset_ok = false;
        tb->mode = M_IN_TABLE;
        return;
    case AU_ATOM_area: case AU_ATOM_br: case AU_ATOM_embed: case AU_ATOM_img: case AU_ATOM_keygen:
    case AU_ATOM_wbr:
        reconstruct_afe(tb);
        insert_html(tb, t);
        pop(tb);
        tb->frameset_ok = false;
        return;
    case AU_ATOM_input:
        if (is_html(tb->context, AU_ATOM_select))
            return;
        if (in_scope(tb, AU_ATOM_select, SCOPE_DEFAULT))
            pop_until_tag(tb, AU_ATOM_select);
        reconstruct_afe(tb);
        insert_html(tb, t);
        pop(tb);
        if (!has_attr_ci(t, AU_ATOM_type, "hidden"))
            tb->frameset_ok = false;
        return;
    case AU_ATOM_param: case AU_ATOM_source: case AU_ATOM_track:
        insert_html(tb, t);
        pop(tb);
        return;
    case AU_ATOM_hr:
        close_p_if_in_button_scope(tb);
        if (in_scope(tb, AU_ATOM_select, SCOPE_DEFAULT))
            implied_end_tags(tb, 0);
        insert_html(tb, t);
        pop(tb);
        tb->frameset_ok = false;
        return;
    case AU_ATOM_image:
        t->name = AU_ATOM_img;
        dispatch(tb, t);
        return;
    case AU_ATOM_textarea:
        insert_html(tb, t);
        tb->skip_lf = true;
        html_tok_set_state(tb->tok, HTML_STATE_RCDATA);
        tb->orig_mode = tb->mode;
        tb->frameset_ok = false;
        tb->mode = M_TEXT;
        return;
    case AU_ATOM_xmp:
        close_p_if_in_button_scope(tb);
        reconstruct_afe(tb);
        tb->frameset_ok = false;
        generic_text(tb, t, HTML_STATE_RAWTEXT);
        return;
    case AU_ATOM_iframe:
        tb->frameset_ok = false;
        generic_text(tb, t, HTML_STATE_RAWTEXT);
        return;
    case AU_ATOM_noembed:
        generic_text(tb, t, HTML_STATE_RAWTEXT);
        return;
    case AU_ATOM_select:
        if (is_html(tb->context, AU_ATOM_select))
            return;
        if (in_scope(tb, AU_ATOM_select, SCOPE_DEFAULT)) {
            pop_until_tag(tb, AU_ATOM_select);
            return;
        }
        reconstruct_afe(tb);
        insert_html(tb, t);
        tb->frameset_ok = false;
        return;
    case AU_ATOM_option:
        if (in_scope(tb, AU_ATOM_select, SCOPE_DEFAULT))
            implied_end_tags(tb, AU_ATOM_optgroup);
        else if (is_html(cur(tb), AU_ATOM_option))
            pop(tb);
        reconstruct_afe(tb);
        insert_html(tb, t);
        return;
    case AU_ATOM_optgroup:
        if (in_scope(tb, AU_ATOM_select, SCOPE_DEFAULT))
            implied_end_tags(tb, 0);
        else if (is_html(cur(tb), AU_ATOM_option))
            pop(tb);
        reconstruct_afe(tb);
        insert_html(tb, t);
        return;
    case AU_ATOM_rb: case AU_ATOM_rtc:
        if (in_scope(tb, AU_ATOM_ruby, SCOPE_DEFAULT))
            implied_end_tags(tb, 0);
        insert_html(tb, t);
        return;
    case AU_ATOM_rp: case AU_ATOM_rt:
        if (in_scope(tb, AU_ATOM_ruby, SCOPE_DEFAULT))
            implied_end_tags(tb, AU_ATOM_rtc);
        insert_html(tb, t);
        return;
    case AU_ATOM_math:
        reconstruct_afe(tb);
        adjust_mathml_attrs(t);
        adjust_foreign_attrs(t);
        insert_foreign(tb, t, AU_NS_MATHML);
        if (t->self_closing)
            pop(tb);
        return;
    case AU_ATOM_svg:
        reconstruct_afe(tb);
        adjust_svg_attrs(t);
        adjust_foreign_attrs(t);
        insert_foreign(tb, t, AU_NS_SVG);
        if (t->self_closing)
            pop(tb);
        return;
    case AU_ATOM_caption: case AU_ATOM_col: case AU_ATOM_colgroup: case AU_ATOM_frame: case AU_ATOM_head:
    case AU_ATOM_tbody: case AU_ATOM_td: case AU_ATOM_tfoot: case AU_ATOM_th: case AU_ATOM_thead:
    case AU_ATOM_tr:
        return;
    default:
        if (n == AU_ATOM_noscript && tb->scripting) {
            generic_text(tb, t, HTML_STATE_RAWTEXT);
            return;
        }
        reconstruct_afe(tb);
        insert_html(tb, t);
        return;
    }
}

static void in_body_end(HtmlTreeBuilder *tb, HtmlToken *t)
{
    AuAtom n = t->name;
    switch (n) {
    case AU_ATOM_template:
        in_head(tb, t);
        return;
    case AU_ATOM_body:
    case AU_ATOM_html:
        if (!in_scope(tb, AU_ATOM_body, SCOPE_DEFAULT))
            return;
        tb->mode = M_AFTER_BODY;
        if (n == AU_ATOM_html)
            dispatch(tb, t);
        return;
    case AU_ATOM_address: case AU_ATOM_article: case AU_ATOM_aside: case AU_ATOM_blockquote:
    case AU_ATOM_button: case AU_ATOM_center: case AU_ATOM_details: case AU_ATOM_dialog: case AU_ATOM_dir:
    case AU_ATOM_div: case AU_ATOM_dl: case AU_ATOM_fieldset: case AU_ATOM_figcaption: case AU_ATOM_figure:
    case AU_ATOM_footer: case AU_ATOM_header: case AU_ATOM_hgroup: case AU_ATOM_listing: case AU_ATOM_main:
    case AU_ATOM_menu: case AU_ATOM_nav: case AU_ATOM_ol: case AU_ATOM_pre: case AU_ATOM_search:
    case AU_ATOM_section: case AU_ATOM_select: case AU_ATOM_summary: case AU_ATOM_ul:
        if (!in_scope(tb, n, SCOPE_DEFAULT))
            return;
        implied_end_tags(tb, 0);
        pop_until_tag(tb, n);
        return;
    case AU_ATOM_form:
        if (!parsing_template(tb)) {
            AuElement *node = tb->form;
            if (node)
                au_node_ref(&node->node);
            set_pointer(&tb->form, NULL);
            if (!node)
                return;
            if (element_in_scope(tb, node)) {
                implied_end_tags(tb, 0);
                stack_remove(tb, node);
            }
            au_node_unref(&node->node);
        } else {
            if (!in_scope(tb, AU_ATOM_form, SCOPE_DEFAULT))
                return;
            implied_end_tags(tb, 0);
            pop_until_tag(tb, AU_ATOM_form);
        }
        return;
    case AU_ATOM_p:
        if (!in_scope(tb, AU_ATOM_p, SCOPE_BUTTON))
            insert_html_tag(tb, AU_ATOM_p);
        close_p(tb);
        return;
    case AU_ATOM_li:
        if (!in_scope(tb, AU_ATOM_li, SCOPE_LIST))
            return;
        implied_end_tags(tb, AU_ATOM_li);
        pop_until_tag(tb, AU_ATOM_li);
        return;
    case AU_ATOM_dd: case AU_ATOM_dt:
        if (!in_scope(tb, n, SCOPE_DEFAULT))
            return;
        implied_end_tags(tb, n);
        pop_until_tag(tb, n);
        return;
    case AU_ATOM_h1: case AU_ATOM_h2: case AU_ATOM_h3: case AU_ATOM_h4: case AU_ATOM_h5: case AU_ATOM_h6:
        if (!heading_in_scope(tb))
            return;
        implied_end_tags(tb, 0);
        while (tb->depth) {
            bool hit = html_flag(cur(tb), F_HEADING);
            pop(tb);
            if (hit)
                break;
        }
        return;
    case AU_ATOM_a: case AU_ATOM_b: case AU_ATOM_big: case AU_ATOM_code: case AU_ATOM_em: case AU_ATOM_font:
    case AU_ATOM_i: case AU_ATOM_nobr: case AU_ATOM_s: case AU_ATOM_small: case AU_ATOM_strike:
    case AU_ATOM_strong: case AU_ATOM_tt: case AU_ATOM_u:
        if (adoption_agency(tb, n))
            any_other_end_tag(tb, n);
        return;
    case AU_ATOM_applet: case AU_ATOM_marquee: case AU_ATOM_object:
        if (!in_scope(tb, n, SCOPE_DEFAULT))
            return;
        implied_end_tags(tb, 0);
        pop_until_tag(tb, n);
        clear_afe_to_marker(tb);
        return;
    case AU_ATOM_br: {
        HtmlToken br;
        memset(&br, 0, sizeof br);
        br.type = HTML_TOK_START;
        br.name = AU_ATOM_br;
        in_body_start(tb, &br);
        return;
    }
    default:
        any_other_end_tag(tb, n);
        return;
    }
}

static void in_template(HtmlTreeBuilder *tb, HtmlToken *t);

static void in_body(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START:
        in_body_start(tb, t);
        return;
    case HTML_TOK_END:
        in_body_end(tb, t);
        return;
    case HTML_TOK_EOF:
        if (tb->ntmpl) {
            in_template(tb, t);
            return;
        }
        stop(tb);
        return;
    }
}

static void text_mode(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (t->type == HTML_TOK_EOF) {
        pop(tb);
        tb->mode = tb->orig_mode;
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END) {
        AuElement *script = cur(tb);
        au_node_ref(&script->node);
        pop(tb);
        tb->mode = tb->orig_mode;
        if (t->name == AU_ATOM_script && is_html(script, AU_ATOM_script))
            run_script_hook(tb, script);
        au_node_unref(&script->node);
    }
}

static void clear_to_table_context(HtmlTreeBuilder *tb)
{
    while (!ONE_OF(cur(tb)->tag, AU_ATOM_table, AU_ATOM_template, AU_ATOM_html) || cur(tb)->ns != AU_NS_HTML)
        pop(tb);
}

static void clear_to_table_body_context(HtmlTreeBuilder *tb)
{
    while (!ONE_OF(cur(tb)->tag, AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead, AU_ATOM_template, AU_ATOM_html) ||
           cur(tb)->ns != AU_NS_HTML)
        pop(tb);
}

static void clear_to_table_row_context(HtmlTreeBuilder *tb)
{
    while (!ONE_OF(cur(tb)->tag, AU_ATOM_tr, AU_ATOM_template, AU_ATOM_html) || cur(tb)->ns != AU_NS_HTML)
        pop(tb);
}

static void table_anything_else(HtmlTreeBuilder *tb, HtmlToken *t)
{
    bool old = tb->foster;
    tb->foster = true;
    in_body(tb, t);
    tb->foster = old;
}

static void in_table(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START:
        switch (t->name) {
        case AU_ATOM_caption:
            clear_to_table_context(tb);
            afe_push_marker(tb);
            insert_html(tb, t);
            tb->mode = M_IN_CAPTION;
            return;
        case AU_ATOM_colgroup:
            clear_to_table_context(tb);
            insert_html(tb, t);
            tb->mode = M_IN_COLUMN_GROUP;
            return;
        case AU_ATOM_col:
            clear_to_table_context(tb);
            insert_html_tag(tb, AU_ATOM_colgroup);
            tb->mode = M_IN_COLUMN_GROUP;
            dispatch(tb, t);
            return;
        case AU_ATOM_tbody: case AU_ATOM_tfoot: case AU_ATOM_thead:
            clear_to_table_context(tb);
            insert_html(tb, t);
            tb->mode = M_IN_TABLE_BODY;
            return;
        case AU_ATOM_td: case AU_ATOM_th: case AU_ATOM_tr:
            clear_to_table_context(tb);
            insert_html_tag(tb, AU_ATOM_tbody);
            tb->mode = M_IN_TABLE_BODY;
            dispatch(tb, t);
            return;
        case AU_ATOM_table:
            if (!in_scope(tb, AU_ATOM_table, SCOPE_TABLE))
                return;
            pop_until_tag(tb, AU_ATOM_table);
            reset_mode(tb);
            dispatch(tb, t);
            return;
        case AU_ATOM_style: case AU_ATOM_script: case AU_ATOM_template:
            in_head(tb, t);
            return;
        case AU_ATOM_input:
            if (!has_attr_ci(t, AU_ATOM_type, "hidden"))
                break;
            insert_html(tb, t);
            pop(tb);
            return;
        case AU_ATOM_form: {
            if (tb->form && !parsing_template(tb))
                return;
            AuElement *e = insert_html(tb, t);
            if (!parsing_template(tb))
                set_pointer(&tb->form, e);
            pop(tb);
            return;
        }
        default:
            break;
        }
        break;
    case HTML_TOK_END:
        switch (t->name) {
        case AU_ATOM_table:
            if (!in_scope(tb, AU_ATOM_table, SCOPE_TABLE))
                return;
            pop_until_tag(tb, AU_ATOM_table);
            reset_mode(tb);
            return;
        case AU_ATOM_body: case AU_ATOM_caption: case AU_ATOM_col: case AU_ATOM_colgroup: case AU_ATOM_html:
        case AU_ATOM_tbody: case AU_ATOM_td: case AU_ATOM_tfoot: case AU_ATOM_th: case AU_ATOM_thead:
        case AU_ATOM_tr:
            return;
        case AU_ATOM_template:
            in_head(tb, t);
            return;
        default:
            break;
        }
        break;
    case HTML_TOK_EOF:
        in_body(tb, t);
        return;
    }
    table_anything_else(tb, t);
}

static void close_caption(HtmlTreeBuilder *tb)
{
    implied_end_tags(tb, 0);
    pop_until_tag(tb, AU_ATOM_caption);
    clear_afe_to_marker(tb);
    tb->mode = M_IN_TABLE;
}

static void in_caption(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (IS_END(t, AU_ATOM_caption)) {
        if (in_scope(tb, AU_ATOM_caption, SCOPE_TABLE))
            close_caption(tb);
        return;
    }
    if ((t->type == HTML_TOK_START &&
         ONE_OF(t->name, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup, AU_ATOM_tbody, AU_ATOM_td, AU_ATOM_tfoot,
                AU_ATOM_th, AU_ATOM_thead, AU_ATOM_tr)) ||
        IS_END(t, AU_ATOM_table)) {
        if (!in_scope(tb, AU_ATOM_caption, SCOPE_TABLE))
            return;
        close_caption(tb);
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END &&
        ONE_OF(t->name, AU_ATOM_body, AU_ATOM_col, AU_ATOM_colgroup, AU_ATOM_html, AU_ATOM_tbody, AU_ATOM_td,
               AU_ATOM_tfoot, AU_ATOM_th, AU_ATOM_thead, AU_ATOM_tr))
        return;
    in_body(tb, t);
}

static void in_column_group(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (t->name == AU_ATOM_col) {
            insert_html(tb, t);
            pop(tb);
            return;
        }
        if (t->name == AU_ATOM_template) {
            in_head(tb, t);
            return;
        }
        break;
    case HTML_TOK_END:
        if (t->name == AU_ATOM_colgroup) {
            if (!is_html(cur(tb), AU_ATOM_colgroup))
                return;
            pop(tb);
            tb->mode = M_IN_TABLE;
            return;
        }
        if (t->name == AU_ATOM_col)
            return;
        if (t->name == AU_ATOM_template) {
            in_head(tb, t);
            return;
        }
        break;
    case HTML_TOK_EOF:
        in_body(tb, t);
        return;
    }
    if (!is_html(cur(tb), AU_ATOM_colgroup))
        return;
    pop(tb);
    tb->mode = M_IN_TABLE;
    dispatch(tb, t);
}

static void in_table_body(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (IS_START(t, AU_ATOM_tr)) {
        clear_to_table_body_context(tb);
        insert_html(tb, t);
        tb->mode = M_IN_ROW;
        return;
    }
    if (IS_START(t, AU_ATOM_th) || IS_START(t, AU_ATOM_td)) {
        clear_to_table_body_context(tb);
        insert_html_tag(tb, AU_ATOM_tr);
        tb->mode = M_IN_ROW;
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END && ONE_OF(t->name, AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead)) {
        if (!in_scope(tb, t->name, SCOPE_TABLE))
            return;
        clear_to_table_body_context(tb);
        pop(tb);
        tb->mode = M_IN_TABLE;
        return;
    }
    if ((t->type == HTML_TOK_START && ONE_OF(t->name, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup,
                                             AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead)) ||
        IS_END(t, AU_ATOM_table)) {
        if (!table_section_in_table_scope(tb))
            return;
        clear_to_table_body_context(tb);
        pop(tb);
        tb->mode = M_IN_TABLE;
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END && ONE_OF(t->name, AU_ATOM_body, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup,
                                          AU_ATOM_html, AU_ATOM_td, AU_ATOM_th, AU_ATOM_tr))
        return;
    in_table(tb, t);
}

static void in_row(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (IS_START(t, AU_ATOM_th) || IS_START(t, AU_ATOM_td)) {
        clear_to_table_row_context(tb);
        insert_html(tb, t);
        tb->mode = M_IN_CELL;
        afe_push_marker(tb);
        return;
    }
    if (IS_END(t, AU_ATOM_tr)) {
        if (!in_scope(tb, AU_ATOM_tr, SCOPE_TABLE))
            return;
        clear_to_table_row_context(tb);
        pop(tb);
        tb->mode = M_IN_TABLE_BODY;
        return;
    }
    if ((t->type == HTML_TOK_START && ONE_OF(t->name, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup,
                                             AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead, AU_ATOM_tr)) ||
        IS_END(t, AU_ATOM_table)) {
        if (!in_scope(tb, AU_ATOM_tr, SCOPE_TABLE))
            return;
        clear_to_table_row_context(tb);
        pop(tb);
        tb->mode = M_IN_TABLE_BODY;
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END && ONE_OF(t->name, AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead)) {
        if (!in_scope(tb, t->name, SCOPE_TABLE))
            return;
        if (!in_scope(tb, AU_ATOM_tr, SCOPE_TABLE))
            return;
        clear_to_table_row_context(tb);
        pop(tb);
        tb->mode = M_IN_TABLE_BODY;
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END && ONE_OF(t->name, AU_ATOM_body, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup,
                                          AU_ATOM_html, AU_ATOM_td, AU_ATOM_th))
        return;
    in_table(tb, t);
}

static void close_cell(HtmlTreeBuilder *tb)
{
    implied_end_tags(tb, 0);
    while (tb->depth) {
        AuElement *c = cur(tb);
        bool hit = is_html(c, AU_ATOM_td) || is_html(c, AU_ATOM_th);
        pop(tb);
        if (hit)
            break;
    }
    clear_afe_to_marker(tb);
    tb->mode = M_IN_ROW;
}

static void in_cell(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (IS_END(t, AU_ATOM_td) || IS_END(t, AU_ATOM_th)) {
        if (!in_scope(tb, t->name, SCOPE_TABLE))
            return;
        implied_end_tags(tb, 0);
        pop_until_tag(tb, t->name);
        clear_afe_to_marker(tb);
        tb->mode = M_IN_ROW;
        return;
    }
    if (t->type == HTML_TOK_START && ONE_OF(t->name, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup, AU_ATOM_tbody,
                                            AU_ATOM_td, AU_ATOM_tfoot, AU_ATOM_th, AU_ATOM_thead, AU_ATOM_tr)) {
        if (!in_scope(tb, AU_ATOM_td, SCOPE_TABLE) && !in_scope(tb, AU_ATOM_th, SCOPE_TABLE))
            return;
        close_cell(tb);
        dispatch(tb, t);
        return;
    }
    if (t->type == HTML_TOK_END &&
        ONE_OF(t->name, AU_ATOM_body, AU_ATOM_caption, AU_ATOM_col, AU_ATOM_colgroup, AU_ATOM_html))
        return;
    if (t->type == HTML_TOK_END &&
        ONE_OF(t->name, AU_ATOM_table, AU_ATOM_tbody, AU_ATOM_tfoot, AU_ATOM_thead, AU_ATOM_tr)) {
        if (!in_scope(tb, t->name, SCOPE_TABLE))
            return;
        close_cell(tb);
        dispatch(tb, t);
        return;
    }
    in_body(tb, t);
}

static void switch_template_mode(HtmlTreeBuilder *tb, uint8_t m, HtmlToken *t)
{
    tb->ntmpl--;
    push_tmpl_mode(tb, m);
    tb->mode = m;
    dispatch(tb, t);
}

static void in_template(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
    case HTML_TOK_PI:
    case HTML_TOK_DOCTYPE:
        in_body(tb, t);
        return;
    case HTML_TOK_START:
        switch (t->name) {
        case AU_ATOM_base: case AU_ATOM_basefont: case AU_ATOM_bgsound: case AU_ATOM_link: case AU_ATOM_meta:
        case AU_ATOM_noframes: case AU_ATOM_script: case AU_ATOM_style: case AU_ATOM_template:
        case AU_ATOM_title:
            in_head(tb, t);
            return;
        case AU_ATOM_caption: case AU_ATOM_colgroup: case AU_ATOM_tbody: case AU_ATOM_tfoot:
        case AU_ATOM_thead:
            switch_template_mode(tb, M_IN_TABLE, t);
            return;
        case AU_ATOM_col:
            switch_template_mode(tb, M_IN_COLUMN_GROUP, t);
            return;
        case AU_ATOM_tr:
            switch_template_mode(tb, M_IN_TABLE_BODY, t);
            return;
        case AU_ATOM_td: case AU_ATOM_th:
            switch_template_mode(tb, M_IN_ROW, t);
            return;
        default:
            switch_template_mode(tb, M_IN_BODY, t);
            return;
        }
    case HTML_TOK_END:
        if (t->name == AU_ATOM_template)
            in_head(tb, t);
        return;
    case HTML_TOK_EOF:
        /* Each open template is closed and the token processed again.
         * While template insertion modes remain, every mode's end-of-file
         * rule comes back here, so the templates are closed in a loop:
         * processing the token again by a call nested once per open
         * template, and a page can leave thousands open. */
        do {
            if (!template_open(tb)) {
                stop(tb);
                return;
            }
            pop_until_tag(tb, AU_ATOM_template);
            clear_afe_to_marker(tb);
            tb->ntmpl--;
            reset_mode(tb);
        } while (tb->ntmpl > 0);
        dispatch(tb, t);
        return;
    }
}

static void after_body(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment_at(tb, t, &tb->stack[0]->node);
        return;
    case HTML_TOK_PI:
        insert_pi_at(tb, t, &tb->stack[0]->node);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        break;
    case HTML_TOK_END:
        if (t->name == AU_ATOM_html) {
            if (!tb->context)
                tb->mode = M_AFTER_AFTER_BODY;
            return;
        }
        break;
    case HTML_TOK_EOF:
        stop(tb);
        return;
    }
    tb->mode = M_IN_BODY;
    dispatch(tb, t);
}

static void in_frameset(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
        } else if (t->name == AU_ATOM_frameset) {
            insert_html(tb, t);
        } else if (t->name == AU_ATOM_frame) {
            insert_html(tb, t);
            pop(tb);
        } else if (t->name == AU_ATOM_noframes) {
            in_head(tb, t);
        }
        return;
    case HTML_TOK_END:
        if (t->name == AU_ATOM_frameset) {
            if (tb->depth == 1)
                return;
            pop(tb);
            if (!tb->context && !is_html(cur(tb), AU_ATOM_frameset))
                tb->mode = M_AFTER_FRAMESET;
        }
        return;
    case HTML_TOK_EOF:
        stop(tb);
        return;
    default:
        return;
    }
}

static void after_frameset(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html)
            in_body(tb, t);
        else if (t->name == AU_ATOM_noframes)
            in_head(tb, t);
        return;
    case HTML_TOK_END:
        if (t->name == AU_ATOM_html)
            tb->mode = M_AFTER_AFTER_FRAMESET;
        return;
    case HTML_TOK_EOF:
        stop(tb);
        return;
    default:
        return;
    }
}

static void after_after(HtmlTreeBuilder *tb, HtmlToken *t, bool frameset)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment_at(tb, t, &tb->doc->node);
        return;
    case HTML_TOK_PI:
        insert_pi_at(tb, t, &tb->doc->node);
        return;
    case HTML_TOK_DOCTYPE:
        in_body(tb, t);
        return;
    case HTML_TOK_START:
        if (t->name == AU_ATOM_html) {
            in_body(tb, t);
            return;
        }
        if (frameset && t->name == AU_ATOM_noframes) {
            in_head(tb, t);
            return;
        }
        break;
    case HTML_TOK_EOF:
        stop(tb);
        return;
    }
    if (frameset)
        return;
    tb->mode = M_IN_BODY;
    dispatch(tb, t);
}

static void process_html(HtmlTreeBuilder *tb, int mode, HtmlToken *t)
{
    if (t->type == HTML_TOK_CHARS) {
        /* Only reached when foreign content hands a token back. */
        chars_html(tb, t->data, t->len);
        return;
    }
    switch (mode) {
    case M_INITIAL: initial(tb, t); break;
    case M_BEFORE_HTML: before_html(tb, t); break;
    case M_BEFORE_HEAD: before_head(tb, t); break;
    case M_IN_HEAD: in_head(tb, t); break;
    case M_IN_HEAD_NOSCRIPT: in_head_noscript(tb, t); break;
    case M_AFTER_HEAD: after_head(tb, t); break;
    case M_IN_BODY: in_body(tb, t); break;
    case M_TEXT: text_mode(tb, t); break;
    case M_IN_TABLE: in_table(tb, t); break;
    case M_IN_TABLE_TEXT:
        flush_table_text(tb);
        tb->mode = tb->orig_mode;
        dispatch(tb, t);
        break;
    case M_IN_CAPTION: in_caption(tb, t); break;
    case M_IN_COLUMN_GROUP: in_column_group(tb, t); break;
    case M_IN_TABLE_BODY: in_table_body(tb, t); break;
    case M_IN_ROW: in_row(tb, t); break;
    case M_IN_CELL: in_cell(tb, t); break;
    case M_IN_TEMPLATE: in_template(tb, t); break;
    case M_AFTER_BODY: after_body(tb, t); break;
    case M_IN_FRAMESET: in_frameset(tb, t); break;
    case M_AFTER_FRAMESET: after_frameset(tb, t); break;
    case M_AFTER_AFTER_BODY: after_after(tb, t, false); break;
    case M_AFTER_AFTER_FRAMESET: after_after(tb, t, true); break;
    }
}

/* ---- Foreign content ---- */

static bool breaks_out(const HtmlToken *t)
{
    if (t->type == HTML_TOK_END)
        return t->name == AU_ATOM_br || t->name == AU_ATOM_p;
    switch (t->name) {
    case AU_ATOM_b: case AU_ATOM_big: case AU_ATOM_blockquote: case AU_ATOM_body: case AU_ATOM_br:
    case AU_ATOM_center: case AU_ATOM_code: case AU_ATOM_dd: case AU_ATOM_div: case AU_ATOM_dl: case AU_ATOM_dt:
    case AU_ATOM_em: case AU_ATOM_embed: case AU_ATOM_h1: case AU_ATOM_h2: case AU_ATOM_h3: case AU_ATOM_h4:
    case AU_ATOM_h5: case AU_ATOM_h6: case AU_ATOM_head: case AU_ATOM_hr: case AU_ATOM_i: case AU_ATOM_img:
    case AU_ATOM_li: case AU_ATOM_listing: case AU_ATOM_menu: case AU_ATOM_meta: case AU_ATOM_nobr:
    case AU_ATOM_ol: case AU_ATOM_p: case AU_ATOM_pre: case AU_ATOM_ruby: case AU_ATOM_s: case AU_ATOM_small:
    case AU_ATOM_span: case AU_ATOM_strong: case AU_ATOM_strike: case AU_ATOM_sub: case AU_ATOM_sup:
    case AU_ATOM_table: case AU_ATOM_tt: case AU_ATOM_u: case AU_ATOM_ul: case AU_ATOM_var:
        return true;
    case AU_ATOM_font:
        return token_has_attr(t, AU_ATOM_color) || token_has_attr(t, AU_ATOM_face) ||
               token_has_attr(t, AU_ATOM_size);
    default:
        return false;
    }
}

static void svg_script_end(HtmlTreeBuilder *tb)
{
    AuElement *script = cur(tb);
    au_node_ref(&script->node);
    pop(tb);
    run_script_hook(tb, script);
    au_node_unref(&script->node);
}

static void foreign(HtmlTreeBuilder *tb, HtmlToken *t)
{
    switch (t->type) {
    case HTML_TOK_COMMENT:
        insert_comment(tb, t);
        return;
    case HTML_TOK_PI:
        insert_pi(tb, t);
        return;
    case HTML_TOK_DOCTYPE:
        return;
    default:
        break;
    }
    if ((t->type == HTML_TOK_START || t->type == HTML_TOK_END) && breaks_out(t)) {
        while (tb->depth) {
            AuElement *c = cur(tb);
            if (is_mathml_text_ip(c) || is_html_ip(c) || c->ns == AU_NS_HTML)
                break;
            pop(tb);
        }
        process_html(tb, tb->mode, t);
        return;
    }
    if (t->type == HTML_TOK_START) {
        AuElement *a = adjusted_cur(tb);
        AuNamespace ns = (AuNamespace)a->ns;
        if (ns == AU_NS_MATHML)
            adjust_mathml_attrs(t);
        if (ns == AU_NS_SVG) {
            t->name = adjust_svg_tag(t->name);
            adjust_svg_attrs(t);
        }
        adjust_foreign_attrs(t);
        insert_foreign(tb, t, ns);
        if (t->self_closing) {
            if (t->name == AU_ATOM_script && cur(tb)->ns == AU_NS_SVG)
                svg_script_end(tb);
            else
                pop(tb);
        }
        return;
    }
    if (t->type == HTML_TOK_END) {
        AuElement *c = cur(tb);
        if (t->name == AU_ATOM_script && c->ns == AU_NS_SVG && c->tag == AU_ATOM_script) {
            svg_script_end(tb);
            return;
        }
        const char *name = au_atom_str(t->name);
        size_t nlen = au_atom_len(t->name);
        for (uint32_t i = tb->depth; i-- > 0;) {
            AuElement *node = tb->stack[i];
            if (i == 0)
                return;
            if (au_ieq(au_atom_str(node->tag), au_atom_len(node->tag), name, nlen)) {
                pop_until_el(tb, node);
                return;
            }
            if (i >= 1 && tb->stack[i - 1]->ns == AU_NS_HTML) {
                process_html(tb, tb->mode, t);
                return;
            }
        }
    }
}

/* ---- The dispatcher ---- */

static void dispatch(HtmlTreeBuilder *tb, HtmlToken *t)
{
    if (tb->stopped)
        return;
    if (t->type == HTML_TOK_CHARS) {
        process_chars(tb, t->data, t->len);
        return;
    }
    AuElement *a = adjusted_cur(tb);
    bool html = !a || a->ns == AU_NS_HTML || t->type == HTML_TOK_EOF;
    if (!html && t->type == HTML_TOK_START) {
        if (is_mathml_text_ip(a) && t->name != AU_ATOM_mglyph && t->name != AU_ATOM_malignmark)
            html = true;
        else if (a->ns == AU_NS_MATHML && a->tag == AU_ATOM_annotation_xml && t->name == AU_ATOM_svg)
            html = true;
        else if (is_html_ip(a))
            html = true;
    }
    if (html)
        process_html(tb, tb->mode, t);
    else
        foreign(tb, t);
}

void html_tb_emit(void *p, HtmlToken *t)
{
    HtmlTreeBuilder *tb = p;
    if (t->type != HTML_TOK_CHARS)
        tb->skip_lf = false;
    dispatch(tb, t);
}

bool html_tb_cdata_ok(void *p)
{
    HtmlTreeBuilder *tb = p;
    AuElement *a = adjusted_cur(tb);
    return a && a->ns != AU_NS_HTML;
}

/* ---- Setup ---- */

HtmlTreeBuilder *html_tb_new(AuDocument *doc, HtmlTokenizer *tok, bool scripting)
{
    HtmlTreeBuilder *tb = au_calloc(sizeof *tb);
    tb->doc = doc;
    tb->node_doc = doc;
    tb->tok = tok;
    tb->scripting = scripting;
    tb->frameset_ok = true;
    tb->mode = M_INITIAL;
    tb->tag_count_size = AU_ATOM_STATIC_COUNT;
    tb->tag_count = au_calloc(AU_ATOM_STATIC_COUNT * sizeof *tb->tag_count);
    tb->afe_tag_count = au_calloc(AU_ATOM_STATIC_COUNT * sizeof *tb->afe_tag_count);
    au_buf_init(&tb->pending);
    return tb;
}

void html_tb_set_fragment_context(HtmlTreeBuilder *tb, AuElement *context)
{
    tb->context = context;
    au_node_ref(&context->node);
    tb->node_doc = context->node.doc;
    AuElement *root = create_element(tb, AU_ATOM_html, AU_NS_HTML, NULL);
    au_node_append(&tb->doc->node, &root->node);
    push(tb, root);
    au_node_unref(&root->node);
    if (is_html(context, AU_ATOM_template))
        push_tmpl_mode(tb, M_IN_TEMPLATE);
    reset_mode(tb);
    for (AuNode *n = &context->node; n; n = n->parent) {
        if (n->type == AU_NODE_ELEMENT && is_html(E(n), AU_ATOM_form)) {
            set_pointer(&tb->form, E(n));
            break;
        }
    }
}

AuNode *html_tb_fragment_root(HtmlTreeBuilder *tb)
{
    return &au_document_element(tb->doc)->node;
}

void html_tb_set_script_hook(HtmlTreeBuilder *tb, void (*hook)(void *ctx, AuElement *script), void *ctx)
{
    tb->script_hook = hook;
    tb->script_ctx = ctx;
}

void html_tb_free(HtmlTreeBuilder *tb)
{
    while (tb->depth)
        pop(tb);
    while (tb->afe_tail)
        afe_free(tb, tb->afe_tail);
    set_pointer(&tb->head, NULL);
    set_pointer(&tb->form, NULL);
    if (tb->context)
        au_node_unref(&tb->context->node);
    au_free(tb->stack);
    au_free(tb->set);
    au_free(tb->tag_count);
    au_free(tb->bhead);
    au_free(tb->btail);
    au_free((void *)tb->afe_map.keys);
    au_free(tb->afe_map.vals);
    au_free(tb->afe_tag_count);
    au_free(tb->tmpl_modes);
    au_buf_free(&tb->pending);
    au_free(tb);
}
