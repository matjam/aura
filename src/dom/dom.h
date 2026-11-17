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
    uint8_t type;  /* AuNodeType */
    uint8_t flags; /* AU_NODEF_* (see "Additions for script") */
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
    AuNode *shadow_root;      /* an AuShadowRoot attached to this element, or NULL (referenced) */
    void *control;            /* form control state (an AuControl, control.h), or NULL */
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
    /* Additions for script (see the end of this file). */
    uint32_t version;          /* incremented by every tree, attribute and text change */
    const struct AuDomHooks *hooks; /* mutation observers of this document, or NULL */
    void *hooks_ctx;
    /* For src/css: the class names and ids that were added to or removed
     * from any element since the last style pass, which clears the list.
     * style_names_n above AU_DOC_STYLE_NAMES means more changed than fit:
     * which ones is then unknown. */
    AuAtom style_names[24];
    uint8_t style_names_n;
    /* An element of this document has had the inert attribute: until
     * then au_node_inert looks only for a modal dialog. */
    bool has_inert;
    /* The top layer (see "The top layer" below), bottom first. */
    AuElement **top_layer;
    uint32_t ntop, top_cap;
    /* An element of this document has had a shadow root: until then
     * style, layout and the event paths walk the plain tree (the flat
     * tree is the same). */
    bool has_shadow;
    /* How many declarative shadow roots the parser has made (script finds
     * new ones when it changes, for slotchange). */
    uint32_t declarative_roots;
    /* Manual slot assignments (slot.assign()), without references: a
     * pair goes when the node or the slot is freed (see "Shadow DOM"
     * below). */
    struct AuManualSlot *manual;
    uint32_t nmanual, manual_cap;
};
#define AU_DOC_STYLE_NAMES 24

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
/* What frees an element's computed style (AuElement.style) when the
 * element is freed: src/css sets it (au_css_init). An element that script
 * removed from its document keeps its style until it is freed. */
void au_dom_set_style_release(void (*release)(void *style));

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
 * selected attribute once AU_STATE_CHECKED_SET is on. AU_STATE_MODAL is
 * a dialog opened with showModal(): it matches :modal and layout paints
 * it in the top layer. AU_STATE_POPOVER_OPEN is a showing popover: it
 * matches :popover-open and is in the top layer too. The bits from
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
    AU_STATE_MODAL = 1u << 10,  /* a modal dialog, in the top layer */
    AU_STATE_POPOVER_OPEN = 1u << 11, /* a showing popover, in the top layer */
    AU_STATE_STYLE_DIRTY = 1u << 16,
    AU_STATE_SUBTREE_DIRTY = 1u << 17,
    AU_STATE_STYLE_SELF_DIRTY = 1u << 18 /* restyle the element; its subtree only if its style changes */
};
void au_element_set_state(AuElement *el, uint32_t bits, bool on);

/* ---- The top layer and inertness ----
 * The top layer (HTML, CSS Positioned Layout 4 section 6) is a list per
 * document of the modal dialogs and showing popovers, in the order they
 * were added: layout paints and hit-tests them in that order above the
 * rest of the page, the last on top. The list does not hold references:
 * an element that leaves the document (au_node_remove, or a move) leaves
 * the list, and loses AU_STATE_MODAL and AU_STATE_POPOVER_OPEN.
 *
 * A node is inert (HTML, "inert subtrees") when it is in an element with
 * the inert attribute, or when the document has a modal dialog and the
 * node is not that dialog or inside it (the topmost modal dialog of the
 * top layer). Inert nodes are not hit, focused, selected or found. */

/* Adds el on top of its document's top layer (moves it there if it is
 * in the list already). The caller sets the state bit. */
void au_top_layer_add(AuElement *el);
/* Takes el out of the list; nothing when it is not in it. */
void au_top_layer_remove(AuElement *el);
/* el's place in its document's top layer (0 is the bottom), or -1. */
int au_top_layer_index(const AuElement *el);
/* The topmost modal dialog of doc's top layer, or NULL. */
AuElement *au_document_modal(const AuDocument *doc);
/* Whether n is inert. */
bool au_node_inert(const AuNode *n);

/* ---- Additions for script ----
 *
 * Mutation hooks. A document's hooks (set with au_document_set_hooks) see
 * every change made through this file's functions, whoever makes it (the
 * parser, script bindings, the document view): MutationObserver, live
 * collections, custom element reactions, shadow slot assignment and style
 * invalidation all hang off them. Hooks run in the middle of a mutation:
 * they must not mutate the tree, free nodes or run script; they record
 * what happened and act later. A document without hooks pays one pointer
 * test per change. AuDocument.version changes on every mutation, hooks or
 * not, so caches (live collections) can tell they are stale. */
typedef struct AuDomHooks {
    /* child was inserted into parent (already linked). */
    void (*inserted)(void *ctx, AuNode *parent, AuNode *child);
    /* child was removed from parent; prev and next were its siblings. */
    void (*removed)(void *ctx, AuNode *parent, AuNode *child, AuNode *prev, AuNode *next);
    /* An attribute is about to change: old is its current value (NULL
     * when absent). After the call the new value is stored. */
    void (*attr_changing)(void *ctx, AuElement *el, AuAtom name, uint8_t ns, const char *old, size_t old_len);
    /* Text, comment or processing instruction data is about to change. */
    void (*text_changing)(void *ctx, AuText *t, const char *old, size_t old_len);
    /* A node with a JavaScript wrapper (AuNode.js set) gained or lost a
     * holder: a parent or a reference. The bindings decide from
     * au_node_held_elsewhere whether the node keeps its wrapper alive. */
    void (*hold_changed)(void *ctx, AuNode *n);
} AuDomHooks;

void au_document_set_hooks(AuDocument *doc, const AuDomHooks *hooks, void *ctx);

/* True when n is held by something besides one reference (its wrapper's):
 * a parent or other references (a shadow host holds one on its shadow
 * root). */
static inline bool au_node_held_elsewhere(const AuNode *n) { return n->parent || n->refs > 1; }

/* AuNode.flags. */
enum {
    AU_NODEF_SHADOW_ROOT = 1u, /* an AuShadowRoot (type AU_NODE_FRAGMENT) */
    AU_NODEF_JS_STRONG = 2u,   /* the node holds its wrapper (owned by src/js) */
    AU_NODEF_JS_MARK = 4u,     /* free for src/js bookkeeping */
    AU_NODEF_MANUAL = 8u       /* in a manual slot pair of its document (freeing it drops them) */
};

/* ---- Shadow DOM ----
 * A shadow root is a document fragment attached to a host element. The host
 * references it (AuElement.shadow_root); the shadow root names its host
 * without a reference. The shadow tree is not among the host's children:
 * the node tree stays a plain tree, and the flat tree (what style and
 * layout should see) is reached through the functions below. */
typedef enum { AU_SHADOW_OPEN = 0, AU_SHADOW_CLOSED = 1 } AuShadowMode;

typedef struct AuShadowRoot {
    AuNode node;
    AuElement *host;
    uint8_t mode;            /* AuShadowMode */
    uint8_t delegates_focus;
    uint8_t manual_slots;    /* slotAssignment "manual" */
    uint8_t clonable, serializable;
    uint8_t declarative;     /* from <template shadowrootmode> */
} AuShadowRoot;

static inline bool au_is_shadow_root(const AuNode *n)
{
    return n && n->type == AU_NODE_FRAGMENT && (n->flags & AU_NODEF_SHADOW_ROOT);
}

/* Attaches a new shadow root to host and returns it (owned by host), or
 * NULL if host already has one. The caller checks attachShadow's rules
 * (valid host names). */
AuShadowRoot *au_element_attach_shadow(AuElement *host, AuShadowMode mode);
/* e may have a shadow root (attachShadow's valid host names: an HTML
 * element with a custom element name or one of a fixed list). */
bool au_element_can_host_shadow(const AuElement *e);
/* The shadow root of el, or NULL. */
static inline AuShadowRoot *au_element_shadow_root(const AuElement *el) { return (AuShadowRoot *)el->shadow_root; }
/* The root of n's tree: a document, shadow root, fragment or detached
 * node. */
AuNode *au_node_root(const AuNode *n);
/* The shadow-including root (through shadow hosts). */
AuNode *au_node_shadow_including_root(const AuNode *n);
/* True when n is in a document (through shadow hosts). */
bool au_node_is_connected(const AuNode *n);
/* The name of a <slot> element (its name attribute, "" when none; "" for
 * any other node). */
const char *au_slot_name_of(const AuNode *n);
/* The slot name a slottable asks for: its slot attribute, "" when none or
 * for text. A <slot> slotted into another shadow tree asks by its slot
 * attribute too. */
const char *au_slottable_name_of(const AuNode *n);
/* The slot n is assigned to, or NULL: n's parent is a shadow host whose
 * shadow root has a <slot> (in tree order the first) with n's slot name,
 * or, with manual assignment, the first slot in tree order whose
 * assign() list holds n. Elements and text are slottables. */
AuElement *au_node_assigned_slot(const AuNode *n);
/* The nodes assigned to slot, in tree order (in assign() order for manual
 * assignment), into out (up to cap); returns the count (may exceed cap). */
size_t au_slot_assigned_nodes(const AuElement *slot, AuNode **out, size_t cap);
/* slot.assign(): replaces the slot's manual list with nodes (elements and
 * text; others are skipped). A node listed by another slot is taken from
 * it. Nothing is assigned until the slot is in a shadow root with manual
 * assignment and the node is a child of its host. */
void au_slot_assign(AuElement *slot, AuNode *const *nodes, size_t n);
typedef struct AuManualSlot {
    AuNode *node;
    AuElement *slot;
} AuManualSlot;
/* For dom.c: n is being freed; drops the pairs that name it. */
void au_manual_slots_removed(AuDocument *d, const AuNode *n);

/* ---- The flat tree ----
 * For style and layout of shadow trees: a shadow host's flat children are
 * its shadow root's children; a <slot>'s flat children are its assigned
 * nodes, or its own children when none are assigned; other nodes' flat
 * children are their children, minus slottables of hosts. A light child
 * of a host that no slot takes, and the fallback children of a slot that
 * has assigned nodes, are not in the flat tree: au_flat_parent still
 * names their parent, so test with au_node_in_flat_tree. */
AuNode *au_flat_first_child(const AuNode *n);
AuNode *au_flat_next_sibling(const AuNode *n);
AuNode *au_flat_parent(const AuNode *n);
/* The flat parent when it is an element, else NULL. */
AuElement *au_flat_parent_element(const AuNode *n);
/* Preorder over root's flat subtree: the node after n, or NULL; the
 * second form skips n's flat subtree. Iterative. */
AuNode *au_flat_next(const AuNode *n, const AuNode *root);
AuNode *au_flat_next_skip(const AuNode *n, const AuNode *root);
/* Whether n's flat parent shows it: false for a light child of a host
 * that no slot takes and for a slot's fallback child while the slot has
 * assigned nodes. Only n's own link is tested, not its ancestors'. */
bool au_node_in_flat_tree(const AuNode *n);
/* The element n's parent pointer reaches in the flat sense: the host of
 * a shadow root. Unlike au_flat_parent, it ignores slots: for retargeting
 * and focus, which follow the shadow-including tree. */
AuNode *au_shadow_including_parent(const AuNode *n);
/* The host of the shadow tree n is in, or NULL in a document tree. */
AuElement *au_node_tree_host(const AuNode *n);
/* Shadow-including preorder within root: an element, then its shadow
 * tree, then its children. Iterative. */
AuNode *au_node_next_including_shadow(const AuNode *n, const AuNode *root);

#endif
