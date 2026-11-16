/* Internal interfaces shared by the files of src/css. Not for use outside
 * src/css and tests/css. */
#ifndef AURA_CSS_INT_H
#define AURA_CSS_INT_H

#include "css.h"

#include <string.h>

/* ---- Small helpers ---- */
#define CSS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Grows *arr (element size sz) so index n fits; *cap doubles. */
void css_grow(void **arr, uint32_t *cap, uint32_t n, size_t sz);
#ifdef AU_MEM_TRACK
#define css_grow(arr, cap, n, sz) (au_mem_site_ = AU_MEM_HERE_, css_grow(arr, cap, n, sz))
#endif
#define CSS_PUSH(arr, n, cap) \
    (css_grow((void **)&(arr), &(cap), (n) + 1, sizeof(*(arr))), &(arr)[(n)++])

/* A bump allocator for temporary trees. Memory is freed by css_arena_reset
 * and css_arena_free only. */
typedef struct CssArenaChunk CssArenaChunk;
typedef struct CssArena {
    CssArenaChunk *head;
    size_t used, cap;
    size_t chunk;     /* the size of the next chunk, 0 for 8 KB */
    size_t chunk_max; /* when set, each new chunk is twice the last, up to this */
} CssArena;
void *css_arena_alloc(CssArena *a, size_t n);
/* The same aligned for pointers only (4 bytes with 32-bit pointers): for
 * records without doubles or 64-bit integers. */
void *css_arena_alloc_ptr(CssArena *a, size_t n);
char *css_arena_strndup(CssArena *a, const char *s, size_t n);
void css_arena_reset(CssArena *a); /* keeps the first chunk */
void css_arena_mem(const CssArena *a, size_t *bytes, size_t *used, size_t *chunks);
/* A place in an arena, and giving back everything allocated since. */
typedef struct CssArenaMark {
    CssArenaChunk *head;
    size_t used;
} CssArenaMark;
static inline CssArenaMark css_arena_mark(const CssArena *a)
{
    CssArenaMark m = {a->head, a->used};
    return m;
}
void css_arena_release(CssArena *a, CssArenaMark m);
void css_arena_free(CssArena *a);

/* ---- Keywords and names (props.c) ---- */
/* The keyword id of a lowercase string, or 0. */
uint16_t css_kw_lookup(const char *s, size_t n);
/* The property id of a lowercase name (aliases resolved), 0 if unknown. */
uint16_t css_prop_lookup(const char *s, size_t n);
bool css_ident_is_custom_prop(const char *s, size_t n); /* starts with "--" */
/* Lowercases ASCII into buf (at most cap bytes); returns false if too long. */
bool css_lower(const char *s, size_t n, char *buf, size_t cap);

/* ---- Tokens (tokenizer.c) ---- */
typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_FUNCTION, /* str = name; children follow until the matching close */
    TK_AT,
    TK_HASH, /* flags TF_ID if an identifier */
    TK_STRING,
    TK_BAD_STRING,
    TK_URL,
    TK_BAD_URL,
    TK_DELIM, /* num = code point */
    TK_NUMBER,
    TK_PERCENT,
    TK_DIMENSION, /* unit; str = unit text */
    TK_WS,
    TK_CDO,
    TK_CDC,
    TK_COLON,
    TK_SEMI,
    TK_COMMA,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE
} CssTokType;

enum { TF_INTEGER = 1, TF_ID = 2, TF_SIGN = 4 /* number written with a sign */ };

typedef struct CssToken {
    uint8_t type;
    uint8_t flags;
    uint16_t kw;   /* keyword id of an ident, function or at-keyword (lowercased), or 0 */
    uint16_t unit; /* AuCssUnit of a dimension */
    uint16_t name_len; /* a function's name length (len holds its block end) */
    uint32_t str;  /* offset of the string in the pool */
    uint32_t len;  /* string length; for blocks and functions, the index just past the matching close */
    float num;
} CssToken;

/* A tokenized input: tokens and their strings. For an opening token
 * (function, ( [ {), len is the index of the token after its matching
 * closing token (or the end). */
typedef struct CssTokens {
    CssToken *t;
    uint32_t n, cap;
    AuBuf pool;
} CssTokens;

void css_tokenize(const char *src, size_t len, CssTokens *out);
/* Recomputes the block end indices (after tokens were copied singly). */
void css_tokens_rematch(CssTokens *tk);
void css_tokens_free(CssTokens *tk);
static inline const char *css_tok_str(const CssTokens *tk, const CssToken *t) { return tk->pool.data + t->str; }
/* The number of a NUMBER, PERCENT or DIMENSION token as it was written
 * ("+0025", "1e3"), without the unit or percent sign; *len is 0 for
 * tokens that were copied (css_tokens_append) rather than tokenized. */
static inline const char *css_tok_num_text(const CssTokens *tk, const CssToken *t, uint32_t *len)
{
    *len = t->type == TK_NUMBER || t->type == TK_PERCENT || t->type == TK_DIMENSION ? t->name_len : 0;
    return tk->pool.data + t->str - *len;
}
/* Where the contents of the block of a { token start in the tokenized
 * text, and where they end for a } token. */
static inline uint32_t css_tok_src(const CssToken *t) { return ((uint32_t)t->unit << 16) | t->name_len; }
/* The string length of a token, functions included. */
static inline uint32_t css_tok_len(const CssToken *t) { return t->type == TK_FUNCTION ? t->name_len : t->len; }
/* Serializes tokens [a, b) back to CSS text. */
void css_tokens_serialize(const CssTokens *tk, uint32_t a, uint32_t b, AuBuf *out);
bool css_tok_is_close(int type);

/* A token list owned by a declaration (custom properties, values with
 * var()). One allocation; tokens index into pool. */
typedef struct CssTokList {
    uint32_t refs;
    CssTokens tk;
    uint8_t has_var; /* contains var() or env() */
    uint8_t has_attr;
} CssTokList;
CssTokList *css_toklist_new(const CssTokens *src, uint32_t a, uint32_t b);
/* Between these, css_toklist_new returns one shared list for equal
 * tokens (a sheet parse). Calls nest. */
void css_toklist_intern_begin(void);
void css_toklist_intern_end(void);
CssTokList *css_toklist_retain(CssTokList *l);
void css_toklist_mem(const CssTokList *l, size_t *bytes, size_t *bytes32, size_t *blocks);
void css_toklist_release(CssTokList *l);
/* Appends tokens [a, b) of src to dst, copying their strings and fixing
 * the block end indices. */
void css_tokens_append(CssTokens *dst, const CssTokens *src, uint32_t a, uint32_t b);

/* ---- Value parsing (values.c) ---- */
typedef struct CssParseCtx {
    CssArena *arena;
    bool quirks;      /* quirks-mode unitless lengths */
    bool in_content;  /* attr() allowed */
    const char *base_url;
} CssParseCtx;

/* Converts tokens [a, b) into a generic value tree: COMMA_LIST if commas
 * appear at top level, else LIST if more than one component, else the
 * component. Returns NULL on a syntax error (bad tokens, unknown
 * function, invalid color or math). Whitespace is dropped. */
AuCssValue *css_parse_value(CssParseCtx *cx, const CssTokens *tk, uint32_t a, uint32_t b);
/* The same for one component starting at *i; advances *i. */
AuCssValue *css_parse_component(CssParseCtx *cx, const CssTokens *tk, uint32_t *i, uint32_t b);
/* True if tokens [a, b) contain var() or env() at any depth. */
bool css_tokens_have_var(const CssTokens *tk, uint32_t a, uint32_t b);

/* Colors */
bool css_color_from_name(const char *s, size_t n, AuColor *out);
bool css_color_from_hex(const char *s, size_t n, AuColor *out);
/* System colors and the like; returns false if kw is not a color keyword. */
bool css_color_keyword(uint16_t kw, AuColor *out, bool *is_current);
/* A value that denotes a color: COLOR node, color keyword, color name. */
bool css_value_to_color(const AuCssValue *v, AuColor *out, bool *current);
/* Folds what can be folded in a calc tree (in place). */
void css_calc_simplify(CssArena *a, AuCssValue *v);

/* Calc: simplification and evaluation. Types are bitmasks of CT_*. */
enum { CT_NUMBER = 0, CT_LENGTH = 1, CT_PERCENT = 2, CT_ANGLE = 4, CT_TIME = 8, CT_FREQ = 16, CT_RES = 32,
       CT_FLEX = 64, CT_INVALID = 0x8000 };
unsigned css_value_type(const AuCssValue *v);
/* kw of the max(x, 0px) node computing wraps around calc() values of
 * properties that cannot be negative; serialization shows only x. */
#define CSS_CALC_CLAMP_WRAPPER 1
/* Conversion of an absolute unit to its canonical one (px, deg, s, Hz,
 * dppx). Returns false for relative units. */
bool css_unit_canonical(int unit, float *factor, int *canon);

/* Building blobs */
size_t css_value_tree_size(const AuCssValue *v);
AuCssBlob *css_blob_pack(const AuCssValue *v, const char *base_url);
/* A blob without a base URL. Between css_blob_intern_begin and
 * css_blob_intern_end (a sheet parse; calls nest) equal values get one
 * shared blob. */
AuCssBlob *css_blob_pack_shared(const AuCssValue *v);
void css_blob_intern_begin(void);
void css_blob_intern_end(void);
AuCssValue *css_value_copy(CssArena *a, const AuCssValue *v);
bool css_value_equal(const AuCssValue *a, const AuCssValue *b);
/* True if computing v needs context (relative units, calc, currentcolor
 * is fine). */
bool css_value_needs_compute(const AuCssValue *v);

AuCssValue *css_new_value(CssArena *a, int type);
AuCssValue *css_new_list(CssArena *a, int type, uint32_t n);
AuCssValue *css_new_kw(CssArena *a, uint16_t kw);
AuCssValue *css_new_num(CssArena *a, int type, float num, int unit);
static inline bool css_is_kw(const AuCssValue *v, uint16_t kw) { return v && v->type == AU_CSSV_KEYWORD && v->kw == kw; }
/* Items of a list value, or the value itself as a one-item list. */
const AuCssValue *css_items(const AuCssValue *v, uint32_t *n);

/* ---- Declarations ---- */
/* DF_QUIRKS_ONLY: valid only in quirks mode (unitless lengths); ignored
 * by the cascade for other documents and by serialization. */
enum { DF_IMPORTANT = 1, DF_NEEDS_COMPUTE = 2, DF_HAS_ATTR = 4, DF_QUIRKS_ONLY = 8 };
enum {
    DK_VALUE = 0, /* u.v: a scalar node (keyword, number, percent, dimension, color) */
    DK_BLOB,      /* u.blob */
    DK_WIDE,      /* CSS-wide keyword in u.v.kw (initial, inherit, unset, revert, revert-layer) */
    DK_VAR,       /* u.toks: value with var(); aux = the shorthand it came from, or 0 */
    DK_CUSTOM     /* u.toks: custom property --aux_atom */
};

typedef struct AuCssDecl {
    uint16_t prop;
    uint8_t flags;
    uint8_t kind;
    uint32_t aux; /* DK_VAR: shorthand id; DK_CUSTOM: name atom */
    union {
        AuCssValue v;
        AuCssBlob *blob;
        CssTokList *toks;
    } u;
} AuCssDecl;

struct AuCssDeclBlock {
    AuCssDecl *d;
    uint32_t n, cap;
};

void css_decl_block_init(AuCssDeclBlock *b);
void css_decl_block_clear(AuCssDeclBlock *b);
void css_decl_release(AuCssDecl *d);
void css_decl_retain(AuCssDecl *d);
/* Parses the contents of a declaration list (tokens [a, b)), appending
 * declarations. Nested rules are not allowed here (inline style,
 * @font-face, keyframes). */
void css_parse_decl_list(const CssTokens *tk, uint32_t a, uint32_t b, AuCssDeclBlock *out, bool quirks,
                         const char *base_url);

/* Parses one declaration's value for prop (tokens [a, b), !important
 * already removed) and appends the resulting longhand declarations.
 * Returns false (appending nothing) if the value is invalid. */
bool css_parse_property(uint16_t prop, const CssTokens *tk, uint32_t a, uint32_t b, bool important,
                        AuCssDeclBlock *out, bool quirks, const char *base_url);

/* ---- Property table (props.c) ---- */
enum { T_KW = 1, T_LEN, T_PX, T_COLOR, T_NUM, T_INT, T_BLOB, T_SPECIAL };
enum { F_INH = 1, F_NONNEG = 2, F_PCT = 4, F_QUIRK = 8 };
enum { G_INH = 0, G_INHR, G_BOX, G_SURR, G_BG, G_FLEX, G_RARE, G_COUNT };
/* Keyword masks for T_LEN, T_PX, T_INT. */
enum {
    LK_AUTO = 1,
    LK_NONE = 2,
    LK_NORMAL = 4,
    LK_SIZE = 8, /* min-content, max-content, fit-content, fit-content(), stretch */
    LK_CONTENT = 16,
    LK_FROM_FONT = 32,
    LK_NUMBER = 64 /* a plain number is allowed (AU_LEN_NUMBER) */
};

typedef struct CssPropInfo CssPropInfo;
/* Validates/normalizes a generic value tree for a T_BLOB or T_SPECIAL
 * property. Returns the tree to store (may be v or a new arena tree) or
 * NULL if invalid. */
typedef AuCssValue *(*CssPropParseFn)(CssParseCtx *cx, uint16_t prop, AuCssValue *v);

struct CssPropInfo {
    const char *name;
    uint8_t type, flags, group;
    uint16_t offset;
    uint16_t extra; /* LK_* mask, or keyword list index */
    const uint16_t *kws; /* T_KW: keyword ids in enum order */
    uint8_t nkws;
    CssPropParseFn parse;
    const char *initial;
};

extern const CssPropInfo css_props[AU_PROP_LONGHAND_END];
const char *css_prop_name(uint16_t prop);

/* Keyword list lookup for T_KW properties (with per-property aliases).
 * Returns the enum value or -1. */
int css_prop_kw_index(uint16_t prop, uint16_t kw);
/* The keyword at an enum index of a keyword property, or 0. */
uint16_t css_prop_kw_at(uint16_t prop, int idx);

/* Logical properties */
enum { LG_MARGIN, LG_PADDING, LG_INSET, LG_BORDER_WIDTH, LG_BORDER_STYLE, LG_BORDER_COLOR, LG_SIZE, LG_MIN_SIZE,
       LG_MAX_SIZE, LG_RADIUS };
enum { LS_BLOCK_START, LS_BLOCK_END, LS_INLINE_START, LS_INLINE_END, LS_INLINE, LS_BLOCK, LS_START_START,
       LS_START_END, LS_END_START, LS_END_END };
/* The physical longhand a flow-relative one maps to. */
uint16_t css_logical_to_physical(uint16_t prop, int writing_mode, int direction);
/* The longhand used to parse a flow-relative property's value. */
uint16_t css_logical_template(uint16_t prop);

/* Shorthands */
typedef bool (*CssShorthandFn)(CssParseCtx *cx, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b,
                               AuCssDeclBlock *out, uint8_t flags);
/* Longhands of a shorthand, terminated by 0. */
const uint16_t *css_shorthand_longhands(uint16_t sh);
bool css_parse_shorthand(CssParseCtx *cx, uint16_t sh, const CssTokens *tk, uint32_t a, uint32_t b,
                         AuCssDeclBlock *out, uint8_t flags);
/* Appends a longhand declaration whose value is the tree v (packed into a
 * blob unless scalar). */
void css_push_decl(AuCssDeclBlock *out, uint16_t prop, const AuCssValue *v, uint8_t flags, const char *base_url);
/* Validates a generic value tree as a longhand value; returns the tree to
 * store or NULL. */
AuCssValue *css_validate_longhand(CssParseCtx *cx, uint16_t prop, AuCssValue *v);
/* The initial value of a longhand as a parsed tree (static storage). */
const AuCssValue *css_initial_value(uint16_t prop);

/* ---- Media queries and @supports (media.c) ---- */
AuCssMediaList *css_media_parse(const CssTokens *tk, uint32_t a, uint32_t b);
AuCssMediaList *css_media_parse_text(const char *text, size_t len);
bool css_media_eval(const AuCssMediaList *m, const AuCssDevice *d);
void css_media_free(AuCssMediaList *m);
/* Moves a parsed list into an arena as one allocation; it is then freed
 * with the arena and never with css_media_free. */
AuCssMediaList *css_media_pack(CssArena *a, AuCssMediaList *m);
const char *css_media_text(const AuCssMediaList *m); /* the query as written */
void css_media_serialize(const AuCssMediaList *m, AuBuf *out);
/* Evaluates an @supports condition in tokens [a, b). */
bool css_supports_eval(const CssTokens *tk, uint32_t a, uint32_t b);

/* ---- Container queries (media.c) ----
 * The prelude of @container: an optional name and a condition over the
 * size features. axes says which a container must allow querying. */
enum { CQ_AXIS_WIDTH = 1, CQ_AXIS_HEIGHT = 2, CQ_AXIS_INLINE = 4, CQ_AXIS_BLOCK = 8 };
typedef struct CssContainerQuery {
    AuAtom name;          /* 0: any container */
    uint8_t axes;         /* CQ_AXIS_* the condition reads */
    AuCssMediaList *cond; /* NULL for a name alone */
} CssContainerQuery;
/* The query container's size and what lengths in the condition need:
 * w and h are its content box, -1 for an axis it cannot be queried in. */
typedef struct CssCqEnv {
    float w, h;
    bool horizontal; /* horizontal writing mode: the inline axis is the width */
    float em, rem;
    const AuCssDevice *dev;
} CssCqEnv;
/* Parses tokens [a, b) into the arena; NULL when the prelude is invalid. */
CssContainerQuery *css_container_parse(CssArena *arena, const CssTokens *tk, uint32_t a, uint32_t b);
bool css_container_eval(const CssContainerQuery *q, const CssCqEnv *e);

/* ---- Selectors (selector.c, match.c) ---- */
typedef struct AuSelector AuSelector;
typedef struct AuSelList AuSelList;

typedef enum {
    SC_TAG = 1,
    SC_UNIVERSAL,
    SC_ID,
    SC_CLASS,
    SC_ATTR,
    SC_PSEUDO,
    SC_NESTING,  /* & : u.list is the parent rule's selectors, NULL for :scope */
    SC_COMBINATOR /* op = combinator; separates compounds (right to left) */
} SelCompType;

enum { COMB_DESCENDANT = 1, COMB_CHILD, COMB_NEXT, COMB_SUBSEQUENT };
enum { ATTR_EXISTS = 1, ATTR_EQ, ATTR_INCLUDES, ATTR_DASH, ATTR_PREFIX, ATTR_SUFFIX, ATTR_SUBSTR };
/* SCF_ON_PART: a pseudo-class after ::part(), matched against the part
 * element (CssMatchCtx.part_el) rather than the originating host. */
enum { SCF_CI = 1, SCF_CS = 2, SCF_NS_ANY = 4, SCF_NS_NONE = 8, SCF_NS_SET = 16, SCF_EXT = 32, SCF_ON_PART = 64 };

typedef enum {
    PC_IS = 1, PC_WHERE, PC_NOT, PC_HAS,
    PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE,
    PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE,
    PC_ROOT, PC_EMPTY, PC_LINK, PC_ANY_LINK, PC_VISITED, PC_LOCAL_LINK,
    PC_HOVER, PC_ACTIVE, PC_FOCUS, PC_FOCUS_VISIBLE, PC_FOCUS_WITHIN,
    PC_CHECKED, PC_INDETERMINATE, PC_DEFAULT, PC_DISABLED, PC_ENABLED, PC_REQUIRED, PC_OPTIONAL,
    PC_READ_ONLY, PC_READ_WRITE, PC_PLACEHOLDER_SHOWN, PC_TARGET, PC_DEFINED, PC_LANG, PC_DIR, PC_SCOPE,
    PC_VALID, PC_INVALID, PC_USER_VALID, PC_USER_INVALID, PC_IN_RANGE, PC_OUT_OF_RANGE, PC_MODAL,
    PC_POPOVER_OPEN, PC_NEVER, /* known pseudo-classes that never match here (:autofill, :fullscreen, ...) */
    /* Shadow trees (CSS Scoping). :host and :host() (u.list: the argument
     * compound, NULL for :host) and :host-context() match only the
     * featureless shadow host (CssMatchCtx.host). PC_SLOTTED is the
     * argument of ::slotted() (u.list), true for CssMatchCtx.slotted;
     * PC_PART holds the names of ::part() (value, space-separated), true
     * when CssMatchCtx.part_names has them all. */
    PC_HOST, PC_HOST_CONTEXT, PC_SLOTTED, PC_PART
} PseudoClass;

/* What a component needs beyond a name: allocated only for the
 * components that have it (SCF_EXT). */
typedef struct CssSelExt {
    AuAtom name;    /* tag (lowercase), attribute name (lowercase) */
    AuAtom name_cs; /* tag or attribute name as written, for non-HTML elements */
    union {
        struct {
            char *s;
            uint32_t len;
        } value; /* attribute value, :lang ranges as a comma-joined string */
        struct {
            int32_t a, b;
            AuSelList *of;
        } nth;
    } u;
} CssSelExt;

/* One component: 8 bytes with 32-bit pointers. Selectors, their
 * components and what hangs off them are allocated in an arena (the
 * sheet's, or the selector list's own) and freed with it. */
typedef struct AuSelComp {
    uint8_t type;  /* SelCompType */
    uint8_t op;    /* attribute operator, pseudo-class, combinator */
    uint8_t flags; /* SCF_* */
    uint8_t ns;    /* namespace (AuNamespace) when SCF_NS_SET */
    union {
        AuAtom name;     /* without SCF_EXT: tag, id, class, attribute name (as written and lowercase
                          * alike), :dir argument (0 ltr, 1 rtl) */
        AuSelList *list; /* :is, :where, :not, :has; & (the parent rule's selectors, NULL for :scope) */
        CssSelExt *ext;  /* with SCF_EXT */
    } u;
} AuSelComp;

static inline AuAtom css_comp_name(const AuSelComp *c) { return c->flags & SCF_EXT ? c->u.ext->name : c->u.name; }
static inline AuAtom css_comp_name_cs(const AuSelComp *c)
{
    return c->flags & SCF_EXT ? c->u.ext->name_cs : c->u.name;
}
/* The attribute value or :lang ranges of a component, NULL without one. */
static inline const char *css_comp_value(const AuSelComp *c, uint32_t *len)
{
    if (!(c->flags & SCF_EXT)) {
        *len = 0;
        return NULL;
    }
    *len = c->u.ext->u.value.len;
    return c->u.ext->u.value.s;
}

/* SELF_NEVER: the selector cannot match an element here (a pseudo-element
 * Aura does not create, or a pseudo-class that is never true, on the
 * subject or an ancestor compound); it is not indexed. A list's flags do
 * not carry it. */
enum { SELF_HAS = 1, SELF_STATE = 2, SELF_NESTED = 4, SELF_ATTR = 8, SELF_NEVER = 16 };

/* 12 bytes with 32-bit pointers. After the components come nhash
 * ancestor filter hashes (css_sel_hashes) and, for
 * AU_CSS_PSEUDO_UNKNOWN_WEBKIT, the pseudo-element's name as written. */
struct AuSelector {
    AuSelComp *comp;       /* right to left */
    unsigned spec : 24;    /* (a << 16) | (b << 8) | c, each clamped to 255 */
    unsigned flags : 8;    /* SELF_* */
    uint16_t ncomp;
    uint8_t pseudo;        /* AuCssPseudo on the subject */
    uint8_t rel_comb : 4;  /* relative selectors (:has): combinator to the anchor, else 0 */
    uint8_t nhash : 4;     /* at most CSS_SEL_HASHES */
};
#define CSS_SEL_HASHES 4
static inline const uint32_t *css_sel_hashes(const AuSelector *s) { return (const uint32_t *)(s->comp + s->ncomp); }
static inline AuAtom css_sel_pseudo_name(const AuSelector *s)
{
    return s->pseudo == AU_CSS_PSEUDO_UNKNOWN_WEBKIT ? (AuAtom)css_sel_hashes(s)[s->nhash] : 0;
}

struct AuSelList {
    unsigned n : 24;
    unsigned flags : 8; /* union of selector flags */
    AuSelector sel[];
};
#define CSS_SELLIST_MAX 0xFFFFFFu

typedef struct CssNamespaces {
    AuAtom default_prefix_ns; /* unused placeholder */
    int default_ns;           /* -1 for none, else AuNamespace */
    AuAtom *prefixes;
    int *nss;
    uint32_t n, cap;
} CssNamespaces;

typedef struct CssSelParseCtx {
    CssArena *arena;         /* where the list is allocated; freed as a whole */
    const AuSelList *parent; /* for & in nested rules (in the same arena), NULL at top level */
    const CssNamespaces *ns;
    bool relative;           /* parse as <relative-selector-list> (:has, nested rules) */
    bool forgiving;          /* :is/:where: drop invalid selectors instead of failing */
    bool nested_rule;        /* a nested style rule: add & where missing */
    int depth;
} CssSelParseCtx;

/* Parses a selector list into cx->arena; NULL if it is invalid (what was
 * allocated for it stays in the arena until the arena is freed). */
AuSelList *css_selector_parse(const CssTokens *tk, uint32_t a, uint32_t b, const CssSelParseCtx *cx);
void css_sellist_serialize(const AuSelList *l, AuBuf *out);
uint32_t css_sellist_max_spec(const AuSelList *l);

/* For :has(): a 64-bit summary of each element's descendants (hashes of
 * their tags, ids, classes and attribute names, and state bits), built
 * once per style pass. A relative selector whose bits are not all in the
 * anchor's summary cannot match, which saves the subtree walk. */
typedef struct CssHasIndex {
    const AuElement **keys;
    uint64_t *masks;
    uint32_t size; /* power of two */
} CssHasIndex;
uint64_t css_has_element_bits(const AuElement *el);
uint64_t css_has_selector_bits(const AuSelector *s);
void css_has_index_build(CssHasIndex *x, const AuNode *root);
void css_has_index_free(CssHasIndex *x);

/* Element state the matcher needs besides the DOM. */
typedef struct CssBloom CssBloom;
typedef struct CssMatchCtx {
    const CssBloom *bloom; /* ancestors of the element being matched, or NULL */
    bool quirks;
    const AuElement *scope; /* :scope; NULL means the root element */
    uint32_t has_used;
    /* The :has() summary, or NULL. An empty index is built from has_root
     * (a document) the first time a :has() needs it; with has_root NULL
     * an empty index is not used. */
    CssHasIndex *has_index;
    const AuNode *has_root;
    /* Shadow trees: the host of the tree whose rules are matched (NULL in
     * the document's), which a combinator reaches from the shadow root's
     * children and which matches only :host-like selectors; the element
     * a ::slotted() rule is matched for (the selector itself is matched
     * against the slot); the element a ::part() rule is matched for and
     * the part names it exposes to the rule's tree. */
    const AuElement *host;
    const AuElement *slotted;
    const AuElement *part_el;
    const AuAtom *part_names;
    uint32_t npart_names;
} CssMatchCtx;

bool css_selector_match(const AuSelector *s, const AuElement *el, const CssMatchCtx *mc);
bool css_sellist_match(const AuSelList *l, const AuElement *el, const CssMatchCtx *mc);

/* Ancestor filter: a counting Bloom filter of the tag, id and class
 * hashes of the current element's ancestors. */
#define CSS_BLOOM_BITS 12
struct CssBloom {
    uint8_t c[1 << CSS_BLOOM_BITS];
};
void css_bloom_clear(CssBloom *b);
void css_bloom_push(CssBloom *b, const AuElement *el, bool quirks);
void css_bloom_pop(CssBloom *b, const AuElement *el, bool quirks);
static inline bool css_bloom_may_contain(const CssBloom *b, uint32_t h)
{
    return b->c[h & ((1 << CSS_BLOOM_BITS) - 1)] && b->c[(h >> CSS_BLOOM_BITS) & ((1 << CSS_BLOOM_BITS) - 1)];
}
uint32_t css_hash_atom(AuAtom a, int salt);
AuAtom css_atom_lower(AuAtom a);

/* ---- Rules (parser.c) ---- */
typedef enum {
    RULE_STYLE = 1,
    RULE_NESTED_DECLS, /* declarations after nested rules: selector & of the parent */
    RULE_MEDIA,
    RULE_SUPPORTS,
    RULE_LAYER_BLOCK,
    RULE_LAYER_STMT,
    RULE_CONTAINER,
    RULE_SCOPE,
    RULE_STARTING_STYLE,
    RULE_IMPORT,
    RULE_FONT_FACE,
    RULE_KEYFRAMES,
    RULE_NAMESPACE,
    RULE_PAGE,
    RULE_PROPERTY
} CssRuleType;

/* A layer name: dotted path of atoms. n == 0 is an anonymous layer. */
typedef struct CssLayerName {
    AuAtom *parts;
    uint32_t n;
} CssLayerName;

/* A rule: 20 bytes with 32-bit pointers. Rules, their selectors, media
 * lists, layer names, condition text and parsed declaration arrays are
 * allocated in the sheet's arena and freed with it.
 *
 * A style rule whose block holds only declarations, and a top-level
 * @media block, stay text (RF_LAZY: a range of the sheet's text) until
 * used: the style rule until it first matches an element
 * (css_rule_decls), the @media block until its query first matches the
 * device (css_rule_children). */
enum {
    RF_OK = 1,   /* @supports: the condition holds; @import: supports() holds. Clear for @scope */
    RF_LAZY = 2, /* c.lazy_len bytes of text at b.off stand for the declarations or the children */
    RF_LAYER = 4 /* @import with a layer: its name is sheet->import_layers[b.index] */
};

typedef struct CssRule CssRule;
struct CssRule {
    uint8_t type;
    uint8_t flags; /* RF_* */
    CssRule *next;
    union {
        CssRule *children; /* without RF_LAZY */
        uint32_t lazy_len;
    } c;
    union {
        AuSelList *sel;        /* style rules; nested declarations (the parent rule's, borrowed) */
        AuCssMediaList *media; /* @media, @import (NULL for all) */
        CssLayerName *layers;  /* @layer: b.nlayers names (a block has one; n == 0 is anonymous) */
        char *text;            /* condition text (@supports, @scope) */
        struct CssContainerQuery *cq; /* @container */
    } u;
    union {
        AuCssDeclBlock *decls; /* style rules, nested declarations (never NULL without RF_LAZY) */
        uint32_t off;          /* RF_LAZY: where the text starts in the sheet's text */
        uint32_t nlayers;      /* @layer */
        uint32_t index;        /* @import */
    } b;
};

/* The declarations of a style or nested-declarations rule of sh, parsing
 * them first if they are still text. */
const AuCssDeclBlock *css_rule_decls(const AuCssSheet *sh, const CssRule *ru);
/* The rules inside a rule of sh, parsing them first if they are still
 * text. */
const CssRule *css_rule_children(const AuCssSheet *sh, const CssRule *ru);
/* For tests: false makes a sheet parse every block when it is parsed. */
extern bool css_lazy_bodies;
/* For tests: true packs a sheet's text as soon as it is parsed, whatever
 * its size. */
extern bool css_pack_at_parse;
#define CSS_TEXT_PIECE 4096u
/* len bytes of the sheet's text at off: a pointer into the text, or,
 * when the text is packed, into the piece inflated last or into tmp. */
const char *css_sheet_text_at(const AuCssSheet *sh, uint32_t off, uint32_t len, AuBuf *tmp);
/* For tests: false makes the rule index hold every occurrence of a sheet
 * that a resolver was given several times. */
extern bool css_index_skip_repeats;

struct AuCssFontFace {
    AuCssDeclBlock descs; /* prop = 0, aux = descriptor atom, kind DK_BLOB */
    const char *base_url;
};

typedef struct CssPropertyRule {
    AuAtom name;
    bool inherits;
    CssTokList *initial;
} CssPropertyRule;

struct AuCssSheet {
    uint32_t refs; /* owners (au_css_sheet_retain); freed with the last */
    uint8_t origin;
    bool quirks;
    bool text_owned;
    /* For a sheet a resolver is given more than once (index_build):
     * has_layers: it names a layer, so an earlier occurrence still sets
     * that layer's place. repeat_matters: an earlier occurrence cannot be
     * left out of the rule index (an anonymous layer, which is a new
     * layer each time, or an @import). */
    bool has_layers;
    bool repeat_matters;
    bool shared; /* listed for other documents to share (au_css_sheet_parse_shared) */
    /* The atom holders of the documents that parsed or shared the sheet
     * (blocks parsed late intern for all of them), and for a listed sheet
     * the atoms it refers to, which a document that shares it holds. */
    AuAtomBits scopes;
    AuAtomSet atoms;
    /* The sheet's source: lazy rules refer to ranges of it, and the
     * document view gives it to the CSSOM. A sheet of 4 GB or more has
     * none (has_text false), and no lazy rules. Once the document has
     * loaded the text is packed (au_css_sheet_pack_text): text is NULL
     * and ztext holds it deflated in pieces of CSS_TEXT_PIECE bytes, each
     * a stream of its own, so that one block's text is read by inflating
     * the piece it is in (css_sheet_text_at). */
    bool has_text;
    bool pack_tried;
    char *text;
    size_t text_len;
    uint8_t *ztext;
    uint32_t *zoff; /* nz + 1 offsets of the pieces in ztext */
    uint32_t nz;
    char *base_url;
    AuCssMediaList *media; /* the sheet's own media list, NULL for all */
    CssArena arena;        /* rules, selectors, media lists, parsed declaration arrays */
    CssRule *rules;
    uint32_t nstyle_rules;
    AuCssImport *imports;
    uint32_t nimports, imports_cap;
    CssRule **import_rules;
    CssLayerName *import_layers; /* one per import; meaningful with RF_LAYER */
    AuCssFontFace *faces;
    uint32_t nfaces, faces_cap;
    AuCssKeyframes *keyframes;
    uint32_t nkeyframes, keyframes_cap;
    CssPropertyRule *props;
    uint32_t nprops, props_cap;
    CssNamespaces ns;
};

/* How a sheet gets its text: a copy, the caller's au_alloc block (the
 * sheet frees it), or the caller's storage that outlives the sheet. */
enum { CSS_TEXT_COPY, CSS_TEXT_TAKE, CSS_TEXT_BORROW };
AuCssSheet *css_sheet_parse(const char *css, size_t len, AuCssOrigin origin, const char *base_url,
                            const char *media, bool quirks, int text_mode);

/* The sizes these records are meant to have with 32-bit pointers (Mac OS
 * 9): a build there fails if a change grows one. */
#define CSS_SIZE_ON_32(T, n) typedef char css_size_of_##T[sizeof(void *) != 4 || sizeof(T) == (n) ? 1 : -1]
CSS_SIZE_ON_32(CssRule, 20);
CSS_SIZE_ON_32(AuSelector, 12);
CSS_SIZE_ON_32(AuSelComp, 8);
CSS_SIZE_ON_32(AuSelList, 4);

/* ---- The UA sheet (ua.c) ---- */
extern const char css_ua_sheet[];
extern const char css_ua_quirks_sheet[];
/* Presentational hints of an element, appended to out; returns true if any. */
bool css_presentational_hints(const AuElement *el, AuCssDeclBlock *out, bool quirks);

/* ---- Computed values (compute.c, style.c) ---- */
void css_style_init_defaults(void);
extern AuStyle css_initial_style;
size_t css_group_size(int g);
void *css_style_group(const AuStyle *s, int g);
void css_group_retain_fields(int g, void *grp);
void css_group_release_fields(int g, void *grp);
void css_group_release(int g, void *grp);
void *css_group_clone(int g, const void *grp);
bool css_group_equal(int g, const void *a, const void *b);
bool css_group_same(int g, const void *a, const void *b);
AuStyle *css_style_new(void);

/* A custom property map: n entries sorted by name. With base set, the map
 * holds only the entries that differ from base (a NULL value hides the
 * base's entry) and lookups go on to base; chains are at most 12 long. */
struct AuCustomProps {
    uint32_t refs;
    uint32_t n;
    struct CssCustomEntry {
        AuAtom name;
        CssTokList *value; /* NULL: guaranteed-invalid (initial) */
    } *e;
    AuCustomProps *base; /* retained, or NULL */
    uint32_t total;      /* entries in the chain, an upper bound of the names */
    uint16_t depth;      /* maps below this one */
};
void css_custom_release(AuCustomProps *c);
AuCustomProps *css_custom_retain(AuCustomProps *c);
CssTokList *css_custom_get(const AuCustomProps *c, AuAtom name);
/* The effective entries of a map and its bases, sorted by name, without
 * hidden ones; values are borrowed. The caller frees the array. */
struct CssCustomEntry *css_custom_flatten(const AuCustomProps *c, uint32_t *n);

/* Context for computing one element's values. */
typedef struct CssComputeCtx {
    AuStyle *style;
    const AuStyle *parent;
    const AuCssDevice *dev;
    const AuStyle *root; /* the root element's style, for rem; NULL when computing the root */
    const AuCssFontPrefs *fp; /* NULL: the defaults */
    AuCssMetricsFn metrics;
    void *metrics_ctx;
    const AuElement *el;
    bool is_root;
    bool quirks;
    float ex, ch; /* cached font metrics, < 0 until asked */
    float font_size_for_em; /* the parent's font size while computing font-size */
    bool font_size_mode;    /* computing font-size: em is the parent's, absolute units zoom */
    uint8_t flags;          /* 2 viewport units, 4 attr(), 8 container units resolved against a container */
    /* For container units: the resolver (NULL: they are small viewport
     * units), and whether the style is a pseudo-element's, whose element
     * is a candidate container. */
    struct AuCssResolver *cq;
    bool cq_self;
} CssComputeCtx;

/* The content-box size of el's nearest query container whose type allows
 * querying axis (CQ_AXIS_WIDTH, _HEIGHT, _INLINE or _BLOCK) in *px;
 * false when there is none or its size is not known (cascade.c). */
bool css_cq_axis_size(CssComputeCtx *c, int axis, float *px);

/* Length conversion against the context (font-relative and viewport). */
float css_length_px(CssComputeCtx *c, float v, int unit);
/* Writes the computed value of a declared value for a longhand into the
 * style (making its group private first). Returns false if the value is
 * invalid at computed-value time. */
bool css_compute_apply(CssComputeCtx *c, uint16_t prop, const AuCssValue *v, AuCssBlob *blob);
void css_compute_inherit(CssComputeCtx *c, uint16_t prop);
void css_compute_initial(CssComputeCtx *c, uint16_t prop);
/* After font-family and font-size: keyword sizes and monospace switches
 * for an element that did not declare font-size (declared false), then
 * the minimum font size. */
void css_compute_font_size_fixup(CssComputeCtx *c, bool declared);
void *css_mut_group(AuStyle *s, int g);
/* Fixups after all longhands: blockification, border widths, overflow. */
void css_compute_finish(CssComputeCtx *c, uint16_t parent_display);
/* Computes a generic value tree into an absolute tree in the arena. */
AuCssValue *css_compute_tree(CssComputeCtx *c, CssArena *a, const AuCssValue *v);
bool css_compute_color(CssComputeCtx *c, const AuCssValue *v, AuColor *out);

/* Serialization of computed longhands (serialize.c). */
void css_serialize_computed(const AuStyle *s, uint16_t prop, AuBuf *out);
void css_serialize_decl_value(const AuCssDecl *d, AuBuf *out);
void css_serialize_string(const char *s, size_t n, AuBuf *out);
void css_serialize_ident(const char *s, size_t n, AuBuf *out);
void css_serialize_number(float v, AuBuf *out);
const char *css_unit_name(int unit);

/* attr() lookup for content (compute.c). */
const char *css_attr_value(const AuElement *el, const char *name, size_t n);

/* HTML names CSS needs that are interned at startup (atoms_html.def is
 * owned by the HTML parser). */
#define CSS_HTML_NAMES(X) \
    X(a, "a") X(area, "area") X(link, "link") X(input, "input") X(textarea, "textarea") \
    X(select, "select") X(option, "option") X(optgroup, "optgroup") X(button, "button") \
    X(fieldset, "fieldset") X(legend, "legend") X(form, "form") X(img, "img") X(table, "table") \
    X(td, "td") X(th, "th") X(tr, "tr") X(tbody, "tbody") X(thead, "thead") X(tfoot, "tfoot") \
    X(caption, "caption") X(col, "col") X(colgroup, "colgroup") X(hr, "hr") X(font, "font") \
    X(center, "center") X(div, "div") X(p, "p") X(li, "li") X(ol, "ol") X(ul, "ul") X(dl, "dl") \
    X(dir, "dir") X(menu, "menu") X(pre, "pre") X(listing, "listing") X(xmp, "xmp") \
    X(iframe, "iframe") X(embed, "embed") X(object, "object") X(video, "video") X(canvas, "canvas") \
    X(svg, "svg") X(details, "details") X(summary, "summary") X(dialog, "dialog") X(href, "href") \
    X(type, "type") X(checked, "checked") X(selected, "selected") X(disabled, "disabled") \
    X(required, "required") X(readonly, "readonly") X(placeholder, "placeholder") X(value, "value") \
    X(lang, "lang") X(width, "width") X(height, "height") X(bgcolor, "bgcolor") X(align, "align") \
    X(valign, "valign") X(border, "border") X(cellpadding, "cellpadding") \
    X(cellspacing, "cellspacing") X(hidden, "hidden") X(color, "color") X(face, "face") \
    X(size, "size") X(nowrap, "nowrap") X(background, "background") X(text, "text") \
    X(start, "start") X(rules, "rules") X(frame, "frame") X(hspace, "hspace") X(vspace, "vspace") \
    X(span, "span") X(noshade, "noshade") X(open, "open") X(multiple, "multiple") X(map, "map") \
    X(marquee, "marquee") X(slot, "slot") X(template, "template") X(body, "body") X(html, "html") \
    X(br, "br") X(wbr, "wbr") X(image, "image") X(math, "math") X(min, "min") X(max, "max") \
    X(step, "step") X(frameset, "frameset") X(noframes, "noframes") X(datalist, "datalist") \
    X(rp, "rp") X(rt, "rt") X(ruby, "ruby") X(bdi, "bdi") X(bdo, "bdo") X(output, "output") \
    X(progress, "progress") X(meter, "meter") X(label, "label") X(nobr, "nobr") X(vlink, "vlink") \
    X(alink, "alink") X(clear, "clear") X(contenteditable, "contenteditable") X(popover, "popover") \
    X(inert, "inert") X(style, "style") X(script, "script") X(head, "head") X(title, "title") \
    X(meta, "meta") X(base, "base") X(noscript, "noscript") X(param, "param") X(source, "source") \
    X(track, "track") X(audio, "audio") X(address, "address") X(blockquote, "blockquote") \
    X(figure, "figure") X(figcaption, "figcaption") X(main, "main") X(nav, "nav") \
    X(section, "section") X(article, "article") X(aside, "aside") X(header, "header") \
    X(footer, "footer") X(h1, "h1") X(h2, "h2") X(h3, "h3") X(h4, "h4") X(h5, "h5") X(h6, "h6") \
    X(hgroup, "hgroup") X(search, "search") X(dd, "dd") X(dt, "dt") X(plaintext, "plaintext") \
    X(b, "b") X(strong, "strong") X(i, "i") X(em, "em") X(cite, "cite") X(var, "var") X(dfn, "dfn") \
    X(code, "code") X(kbd, "kbd") X(samp, "samp") X(tt, "tt") X(u, "u") X(ins, "ins") X(s, "s") \
    X(strike, "strike") X(del, "del") X(sub, "sub") X(sup, "sup") X(small, "small") X(big, "big") \
    X(mark, "mark") X(q, "q") X(abbr, "abbr") X(acronym, "acronym") X(time, "time") X(data, "data") \
    X(blink, "blink") X(tabindex, "tabindex") X(compact, "compact") X(cols, "cols") X(rows, "rows") \
    X(wrap, "wrap") X(reversed, "reversed") X(name, "name") X(xml_lang, "xml:lang") X(char_, "char") \
    X(link_attr, "link") X(dir_attr, "dir")
enum {
#define X(n, s) CSS_HN_##n,
    CSS_HTML_NAMES(X)
#undef X
    CSS_HN_COUNT
};
extern AuAtom css_hn[CSS_HN_COUNT];
#define HN(n) (css_hn[CSS_HN_##n])

/* Keyword atoms */
extern AuAtom css_kw_atoms[AU_K_COUNT];

/* Debug/test hooks */
void css_debug_dump_value(const AuCssValue *v, AuBuf *out);

#endif
