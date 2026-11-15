/* Aura DOM: the node tree the parser builds, CSS matches against, layout
 * reads and JavaScript mutates.
 *
 * Lifetime: a node stays alive while it has a parent or its reference
 * count is above zero. A parent keeps its children alive; au_node_ref is
 * for holders outside the tree (JavaScript wrappers, a pointer kept across
 * a call that may mutate the tree). Constructors return a node with one
 * reference, owned by the caller. The usual pattern:
 *
 *     AuNode *el = au_element_new(doc, AU_ATOM_div, AU_NS_HTML);
 *     au_node_append(parent, el);
 *     au_node_unref(el);          // the tree owns it now
 */
#ifndef AURA_DOM_H
#define AURA_DOM_H

#include "../core/core.h"

typedef enum {
    AU_NODE_ELEMENT = 1,
    AU_NODE_TEXT = 3,
    AU_NODE_PROCESSING_INSTRUCTION = 7, /* an AuProcessingInstruction */
    AU_NODE_COMMENT = 8,
    AU_NODE_DOCUMENT = 9,
    AU_NODE_DOCTYPE = 10,
    AU_NODE_FRAGMENT = 11
} AuNodeType;

typedef enum { AU_NS_HTML, AU_NS_SVG, AU_NS_MATHML } AuNamespace;

/* Attribute namespaces, for foreign content (xlink:href and so on). */
typedef enum { AU_ATTR_NS_NONE, AU_ATTR_NS_XLINK, AU_ATTR_NS_XML, AU_ATTR_NS_XMLNS } AuAttrNamespace;

typedef enum { AU_QUIRKS_NONE, AU_QUIRKS_LIMITED, AU_QUIRKS_FULL } AuQuirksMode;

typedef struct AuNode AuNode;
typedef struct AuDocument AuDocument;

struct AuNode {
    uint8_t type; /* AuNodeType */
    uint32_t refs;
    AuDocument *doc; /* owning document; for the document itself, itself */
    AuNode *parent, *first_child, *last_child, *prev, *next;
    void *js; /* JavaScript wrapper, owned by the bindings */
};

typedef struct AuAttr {
    AuAtom name; /* for namespaced attributes, the qualified name ("xlink:href") */
    uint8_t ns;  /* AuAttrNamespace */
    char *value; /* NUL-terminated UTF-8, never NULL */
    uint32_t len;
} AuAttr;

typedef struct AuElement {
    AuNode node;
    AuAtom tag; /* local name; lowercase for HTML elements */
    uint8_t ns; /* AuNamespace */
    AuAttr *attrs;
    uint32_t nattrs, attr_cap;
    /* Caches kept in step with the id and class attributes. */
    AuAtom id;
    AuAtom *classes;
    uint32_t nclasses;
    AuNode *template_content; /* <template>: a fragment, else NULL */
    void *style;              /* computed style, owned by src/css */
    void *box;                /* layout data, owned by src/layout */
    uint32_t state;           /* AU_STATE_* bits (see the end of this file) */
} AuElement;

/* Text and comment nodes. */
typedef struct AuText {
    AuNode node;
    char *data; /* NUL-terminated UTF-8 */
    size_t len, cap;
} AuText;

typedef struct AuDoctype {
    AuNode node;
    char *name, *public_id, *system_id; /* never NULL; "" when absent */
} AuDoctype;

struct AuDocument {
    AuNode node;
    uint8_t quirks; /* AuQuirksMode */
    char *url;      /* may be NULL */
};

/* ---- Casts. The caller checks node->type first. ---- */
static inline AuElement *au_as_element(AuNode *n) { return (AuElement *)n; }
static inline AuText *au_as_text(AuNode *n) { return (AuText *)n; }
static inline bool au_is_element(const AuNode *n) { return n && n->type == AU_NODE_ELEMENT; }
static inline bool au_is_html_element(const AuNode *n, AuAtom tag)
{
    return n && n->type == AU_NODE_ELEMENT && ((const AuElement *)n)->ns == AU_NS_HTML &&
           ((const AuElement *)n)->tag == tag;
}

/* ---- Construction ---- */
AuDocument *au_document_new(void);
AuNode *au_element_new(AuDocument *doc, AuAtom tag, AuNamespace ns);
AuNode *au_text_new(AuDocument *doc, const char *data, size_t len);
AuNode *au_comment_new(AuDocument *doc, const char *data, size_t len);
AuNode *au_doctype_new(AuDocument *doc, const char *name, const char *public_id, const char *system_id);
AuNode *au_fragment_new(AuDocument *doc);

/* ---- Lifetime ---- */
AuNode *au_node_ref(AuNode *n);
void au_node_unref(AuNode *n);

/* ---- Tree ----
 * Insertion removes the child from its current parent first. These do not
 * check DOM hierarchy rules (cycles, document children); callers that take
 * input from script must. */
void au_node_append(AuNode *parent, AuNode *child);
void au_node_insert_before(AuNode *parent, AuNode *child, AuNode *before); /* before NULL: append */
/* Detaches the node. If nothing else references it, it and its subtree
 * are freed, so ref it first to keep it. */
void au_node_remove(AuNode *child);
/* Moves every child of from to the end of to. */
void au_node_reparent_children(AuNode *from, AuNode *to);

/* Preorder traversal within root: returns the node after n, or NULL. */
AuNode *au_node_next(const AuNode *n, const AuNode *root);
/* The same, skipping n's subtree. */
AuNode *au_node_next_skip(const AuNode *n, const AuNode *root);

/* ---- Attributes ---- */
/* Returns the attribute or NULL. */
AuAttr *au_element_attr(const AuElement *el, AuAtom name);
/* Returns the value or NULL. */
const char *au_element_get_attr(const AuElement *el, AuAtom name);
void au_element_set_attr(AuElement *el, AuAtom name, const char *value, size_t len);
void au_element_set_attr_ns(AuElement *el, AuAtom name, AuAttrNamespace ns, const char *value, size_t len);
void au_element_remove_attr(AuElement *el, AuAtom name);
bool au_element_has_class(const AuElement *el, AuAtom cls);

/* ---- Character data ---- */
void au_text_append(AuText *t, const char *data, size_t len);
void au_text_set(AuText *t, const char *data, size_t len);

/* ---- Document ---- */
/* The <html> element, <head> and <body>, or NULL. */
AuElement *au_document_element(const AuDocument *doc);
AuElement *au_document_head(const AuDocument *doc);
AuElement *au_document_body(const AuDocument *doc);

/* ---- Additions for the HTML parser ---- */

/* Processing instruction node (<?target data?>). The text part holds the
 * data, so au_as_text works on it. */
typedef struct AuProcessingInstruction {
    AuText text;
    AuAtom target;
} AuProcessingInstruction;

AuNode *au_pi_new(AuDocument *doc, AuAtom target, const char *data, size_t len);

/* Appends an attribute without looking for an existing one with the same
 * name. The caller guarantees the name is not already present (the HTML
 * tokenizer removes duplicates), which keeps building an element with n
 * attributes O(n). */
void au_element_append_attr(AuElement *el, AuAtom name, AuAttrNamespace ns, const char *value, size_t len);

/* ---- Element state ----
 * Dynamic state that selectors such as :hover and :checked match. Event
 * and form code sets it with au_element_set_state and then tells the style
 * resolver (au_css_invalidate with AU_CSS_CHANGE_STATE).
 *
 * AU_STATE_HOVER and AU_STATE_ACTIVE set on an element are also set on
 * (and cleared from) its ancestors, as :hover and :active require.
 * AU_STATE_FOCUS sets AU_STATE_FOCUS_WITHIN on the element and its
 * ancestors. AU_STATE_CHECKED is the current checkedness of an input or
 * the selectedness of an option; it is used instead of the checked or
 * selected attribute once AU_STATE_CHECKED_SET is on. The bits from
 * AU_STATE_STYLE_DIRTY up belong to src/css. */
enum {
    AU_STATE_HOVER = 1u << 0,
    AU_STATE_ACTIVE = 1u << 1,
    AU_STATE_FOCUS = 1u << 2,
    AU_STATE_FOCUS_VISIBLE = 1u << 3,
    AU_STATE_FOCUS_WITHIN = 1u << 4,
    AU_STATE_CHECKED = 1u << 5,
    AU_STATE_CHECKED_SET = 1u << 6,
    AU_STATE_INDETERMINATE = 1u << 7,
    AU_STATE_TARGET = 1u << 8,
    AU_STATE_DEFINED = 1u << 9, /* a custom element (name with '-') that is defined */
    AU_STATE_STYLE_DIRTY = 1u << 16,
    AU_STATE_SUBTREE_DIRTY = 1u << 17
};
void au_element_set_state(AuElement *el, uint32_t bits, bool on);

#endif
