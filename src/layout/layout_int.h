/* Layout internals: the box tree, fragments, the formatting-context
 * interface and the helpers every layout file shares.
 *
 * Coordinates. A box that has geometry (an LBlock: block-level boxes,
 * atomic inlines, floats, positioned boxes, flex items) stores its border
 * box relative to the border box of its geometry parent, the nearest
 * ancestor that is an LBlock. Inline boxes and text have no geometry of
 * their own: line layout produces fragments, stored on the block container
 * that owns the inline formatting context (the "IFC root"), relative to its
 * border box. Moving a box therefore moves its whole subtree, which is what
 * lets margin collapsing, relative positioning and incremental layout
 * adjust a box without touching its descendants.
 *
 * Memory. Boxes and their text live in the tree arena, freed in bulk when
 * the box tree is rebuilt. Lines, fragments and stacking lists are heap
 * arrays owned by their box, replaced when that box is laid out again, so
 * a subtree skipped by incremental layout keeps its fragments. */
#ifndef AURA_LAYOUT_INT_H
#define AURA_LAYOUT_INT_H

#include "layout.h"

#include <math.h>
#include <string.h>

/* ---- Arena ---- */
typedef struct LArenaChunk LArenaChunk;
typedef struct LArena {
    LArenaChunk *chunks;
    char *ptr, *end;
    size_t total; /* bytes requested, for statistics */
} LArena;
void ar_init(LArena *a);
void *ar_alloc(LArena *a, size_t n); /* zeroed, 8-byte aligned */
char *ar_strndup(LArena *a, const char *s, size_t n);
void ar_free_all(LArena *a);

/* ---- Limits ----
 * Layout and painting recurse over box nesting. Boxes deeper than
 * L_MAX_DEPTH are laid out as empty and not painted, so a hostile page
 * cannot overflow the stack. Each level costs a few hundred bytes of stack
 * in the deepest path (block -> inline -> atomic inline), so 200 levels fit
 * comfortably in a 256 KB task stack. */
#define L_MAX_DEPTH 200

/* ---- Boxes ---- */
enum { LK_BLOCK, LK_INLINE, LK_TEXT, LK_BR, LK_WBR };

/* Inner display types with a formatting context (index into l_fc_ops). */
enum {
    LI_FLOW,      /* block container: block children or an inline formatting context */
    LI_FLEX,
    LI_GRID,      /* placeholder: laid out as LI_FLOW */
    LI_TABLE,     /* placeholder table layout (table.c) */
    LI_TABLE_ROW_GROUP,
    LI_TABLE_ROW,
    LI_TABLE_CELL,
    LI_REPLACED,  /* no children: images, form controls */
    LI_COUNT
};

/* Replaced content kinds (b->replaced). Form controls are replaced boxes
 * (LI_REPLACED) except LR_BUTTON on a <button> element and LR_TEXTAREA,
 * which are block containers (LI_FLOW) laid out from their content: for
 * those, replaced names the control kind and rep holds its data. */
enum { LR_NONE, LR_IMAGE, LR_TEXT_INPUT, LR_CHECKBOX, LR_RADIO, LR_BUTTON, LR_SELECT, LR_TEXTAREA,
       LR_RANGE, LR_OTHER, LR_DISC, LR_CIRCLE, LR_SQUARE, /* list marker shapes */
       LR_LISTBOX, LR_FILE, LR_COLOR, LR_PROGRESS, LR_METER,
       LR_DISCLOSURE_OPEN, LR_DISCLOSURE_CLOSED /* marker shapes */ };

/* True for the kinds that are form controls. */
static inline bool l_is_control_kind(int r)
{
    return (r >= LR_TEXT_INPUT && r <= LR_RANGE) || (r >= LR_LISTBOX && r <= LR_METER);
}

/* List box rows (LReplaced.label of an LR_LISTBOX): one line per row,
 * each starting with a flags byte 0x40 | LB_*. */
enum { LB_SELECTED = 1, LB_GROUP = 2, LB_INDENT = 4, LB_DISABLED = 8 };

enum {
    LF_ANON = 1u << 0,          /* anonymous box: box-model properties are initial */
    LF_INLINE_LEVEL = 1u << 1,  /* LK_BLOCK: atomic inline (inline-block, replaced inline) */
    LF_FLOAT = 1u << 2,
    LF_ABSPOS = 1u << 3,        /* absolute or fixed */
    LF_FIXED = 1u << 4,
    LF_RELPOS = 1u << 5,        /* relative */
    LF_STICKY = 1u << 6,
    LF_BFC = 1u << 7,           /* establishes an independent formatting context */
    LF_INLINES = 1u << 8,       /* block container whose children are inline-level (IFC root) */
    LF_LAYERED = 1u << 9,       /* painted from a stacking context's layer list */
    LF_STACKING = 1u << 10,     /* creates a stacking context */
    LF_CLIPS = 1u << 11,        /* overflow other than visible: clips descendants */
    LF_SCROLLER = 1u << 12,     /* overflow scroll or auto: a scroll container */
    LF_SPLIT_HEAD = 1u << 13,   /* inline box continued from a previous part: no start edge */
    LF_SPLIT_TAIL = 1u << 14,   /* inline box continued in a later part: no end edge */
    LF_MARKER_OUT = 1u << 15,   /* outside list marker */
    LF_DIRTY = 1u << 16,        /* needs layout */
    LF_CHILD_DIRTY = 1u << 17,  /* a descendant needs layout */
    LF_INTRINSIC_OK = 1u << 18, /* min_c and max_c are valid */
    LF_HAS_LAYOUT = 1u << 19,   /* laid out at least once (cache keys valid) */
    LF_FLEX_ITEM = 1u << 20,
    LF_TRANSFORM = 1u << 21,
    LF_HIDDEN = 1u << 22,       /* visibility hidden or collapse */
    LF_POSITIONED = LF_ABSPOS | LF_RELPOS | LF_STICKY,
    LF_CB = 1u << 23,           /* containing block for absolutely positioned descendants */
    LF_TOO_DEEP = 1u << 24,     /* beyond L_MAX_DEPTH: laid out empty, not painted */
    LF_LIST_ITEM = 1u << 25,
    LF_LINE_CLAMP = 1u << 26,
    LF_ESCAPES = 1u << 27,      /* holds an absolutely positioned box whose containing block is outside it */
    LF_MEASURE_OK = 1u << 28,   /* the measure cache (mk_*, m_res) is valid */
    LF_LEGEND = 1u << 29        /* the rendered legend of its fieldset parent: placed on the top border */
};

typedef struct LBox LBox;
struct LBox {
    LBox *parent, *next, *first, *last;
    AuNode *node;   /* element or text node; the originating element for pseudo boxes */
    AuStyle *style; /* retained; anonymous boxes hold their parent's style */
    uint8_t kind;   /* LK_* */
    uint8_t inner;  /* LI_* */
    uint8_t pseudo; /* AuCssPseudo, 0 for element boxes */
    uint8_t replaced; /* LR_* */
    uint32_t flags; /* LF_* */
};

typedef struct LText {
    LBox b;
    char *text; /* processed: white space collapsed, transformed */
    uint32_t len;
} LText;

/* Fragment kinds. */
enum { FR_TEXT, FR_INLINE, FR_ATOMIC, FR_BR };
enum {
    FRF_START = 1,    /* inline: the box's start edge is on this line */
    FRF_END = 2,      /* inline: the end edge is on this line */
    FRF_HYPHEN = 4,   /* text: ends at a soft hyphen that was taken: draw "-" */
    FRF_ELLIPSIS = 8, /* text: draw an ellipsis after it */
    FRF_HIDDEN = 16   /* cut by text-overflow or line-clamp: not painted */
};

typedef struct LFrag {
    LBox *box;
    float x, y, w, h; /* text and inline: the box's content area (inline: border box); atomic: margin box */
    uint32_t start, len; /* text: byte range in the LText */
    float spacing;    /* text: extra space per space character (justification) */
    float baseline;   /* from y */
    uint8_t kind, flags;
    uint16_t depth;   /* inline nesting depth within the line, for painting order */
} LFrag;

typedef struct LLine {
    float y, h, baseline; /* baseline from the line top */
    float x, w;          /* extent of the content on the line */
    uint32_t first, n;   /* fragments */
} LLine;

/* A stacking context's layered descendants, in painting order: negative
 * z-index first (sorted), then z-index 0/auto in tree order, then
 * positive. */
typedef struct LLayers {
    LBox **items;
    uint32_t n, nneg, nzero;
} LLayers;

/* (Not "LRect": the Mac OS List Manager has a function of that name,
 * visible through Retro68's string.h.) */
typedef struct LArea {
    float x, y, w, h;
} LArea;

/* Replaced element data. */
typedef struct LReplaced {
    float iw, ih;     /* intrinsic size, 0 when unknown */
    float ratio;      /* intrinsic aspect ratio, 0 when none */
    const AuSurface *pixels; /* decoded image or NULL */
    const char *label; /* image alt, button, select or input text; list box rows */
    const char *aux;   /* placeholder of a text control; file input status text */
    uint16_t rows, cols; /* textarea and list box size */
    bool loaded;      /* the image is known (size from the loader) */
    bool placeholder; /* text input: label is the placeholder */
} LReplaced;

typedef struct LBlock {
    LBox b;
    float x, y, w, h;   /* border box, relative to the geometry parent's border box */
    float m[4], p[4], bw[4]; /* used margins, padding, border widths: top, right, bottom, left */
    LArea ov;           /* overflow (painted extent) relative to the border box */
    float baseline;     /* first baseline from the border-box top, NAN if none */
    float last_baseline;
    float scroll_x, scroll_y; /* scroll containers */
    float sw, sh;       /* scrollable content extent from the padding-box origin */
    LLine *lines;
    LFrag *frags;
    uint32_t nlines, nfrags;
    LLayers *layers;    /* stacking contexts */
    LBox *cb;           /* absolutely positioned: the containing block box (NULL: initial) */
    float sx, sy;       /* absolutely positioned: static position in the geometry parent */
    float min_c, max_c; /* intrinsic content-box widths, valid with LF_INTRINSIC_OK */
    float key_w, key_h, key_cbh; /* constraints of the last layout (incremental) */
    float mk_w, mk_h, mk_cbh, m_res; /* measure cache: constraints and resulting border-box height */
    float sticky_dx, sticky_dy;  /* sticky offset applied at the last paint */
    LReplaced *rep;     /* replaced content */
} LBlock;

static inline bool lb_is_block(const LBox *b) { return b->kind == LK_BLOCK; }
static inline LBlock *lb_block(LBox *b) { return (LBlock *)b; }
static inline LText *lb_text(LBox *b) { return (LText *)b; }

/* ---- Fonts and text measurement (text.c) ---- */
typedef struct LFont {
    AuFontFace *face;
    float size;
    AuFontMetrics m;
    uint16_t weight;
    bool italic;
    const AuCssBlob *family;
    struct LFont *next; /* hash chain */
} LFont;

/* ---- The layout context ---- */
typedef struct LWordEntry LWordEntry;
typedef struct LFallback LFallback;

typedef struct LSeg LSeg;
typedef struct LItem LItem;

typedef struct LAbsEntry {
    LBox *box; /* LBlock */
    LBox *cb;  /* containing block box, NULL for the initial containing block */
} LAbsEntry;

typedef struct LFloats LFloats;

struct AuLayout {
    AuLayoutEnv env;
    AuDocument *doc;
    LArena tree;
    LBox *root;        /* the root element's box, or NULL */
    float vw, vh;      /* viewport */
    float doc_w, doc_h;
    bool need_build, need_layout, built_once;
    /* fonts */
    LFont *font_hash[64];
    LArena font_arena;
    LFallback *fallback; /* code point to face cache */
    /* word width cache */
    LWordEntry **words;
    uint32_t nwords, words_cap;
    LArena word_arena;
    /* scratch for inline layout */
    LItem *items;
    uint32_t nitems, items_cap;
    LSeg *segs;
    uint32_t nsegs, segs_cap;
    /* absolutely positioned boxes waiting for their containing block */
    LAbsEntry *abs;
    uint32_t nabs, abs_cap;
    /* statistics */
    AuLayoutStats stats;
    int depth;
    float view_scroll_x, view_scroll_y; /* during paint and hit testing */
    bool need_paint;
    bool need_restack;
    bool full_layout; /* the viewport changed: no subtree may be reused */
};

/* ---- Style access with anonymous boxes ---- */
static inline const AuStyle *lb_style(const LBox *b) { return b->style; }
static inline bool lb_anon(const LBox *b) { return (b->flags & LF_ANON) != 0; }

/* Converts a CSS colour (RGBA, red in the high byte) to a paint colour
 * (ARGB). The two headers use the same type name for different layouts. */
static inline uint32_t l_paint_color(uint32_t css)
{
    return (css >> 8) | (css << 24);
}

/* ---- Shared functions ---- */
/* text.c */
LFont *l_font(AuLayout *L, const AuStyle *s);
float l_measure(AuLayout *L, LFont *f, const AuStyle *s, const char *t, size_t n);
float l_measure_raw(AuLayout *L, LFont *f, const char *t, size_t n);
AuFontFace *l_face_for(AuLayout *L, LFont *f, uint32_t cp);
float l_line_height(const AuStyle *s, LFont *f);
void l_text_free_caches(AuLayout *L);
void l_draw_text(AuLayout *L, AuCanvas *c, LFont *f, const AuStyle *s, const char *t, size_t n, float x, float y,
                 uint32_t color, float spacing);
int l_break_class(uint32_t cp);
enum { BK_NONE, BK_SPACE, BK_IDEO, BK_HYPHEN, BK_ZWSP, BK_SHY, BK_CLOSE, BK_OPEN, BK_NEWLINE, BK_TAB };

/* boxtree.c */
void l_build_tree(AuLayout *L);
void l_free_tree(AuLayout *L);
void l_box_free_layout(LBox *b);
const char *l_attr(const LBox *b, AuAtom name);
bool l_is_tag(const LBox *b, AuAtom tag);
const char *l_val_name(const AuCssValue *v, size_t *len);
const AuCssValue *l_items(const AuCssValue *v, uint32_t *n);

/* Formatting contexts (layout.c dispatches). */
typedef struct LConstraints {
    float avail_w;   /* the box's content-box width (definite) */
    float avail_h;   /* content-box height if definite, else -1 */
    float cb_w, cb_h; /* percentage bases for the box's children (cb_h -1: indefinite) */
} LConstraints;

/* The interface each formatting context implements. layout positions
 * the box's children inside its content box (width c->avail_w) and
 * returns the content height it needs; baselines go in the box. intrinsic
 * returns the min-content and max-content widths of the content box. */
typedef struct LFcOps {
    float (*layout)(AuLayout *L, LBlock *b, const LConstraints *c);
    void (*intrinsic)(AuLayout *L, LBlock *b, float *min_c, float *max_c);
} LFcOps;
extern const LFcOps l_fc_flow, l_fc_flex, l_fc_table, l_fc_replaced;
const LFcOps *l_fc_ops(const LBlock *b);

/* layout.c: sizing of a box under a containing block */
enum { LS_STRETCH, LS_FIT, LS_GIVEN }; /* width mode */
void l_resolve_box_edges(LBlock *b, float cb_w);
float l_used_width(AuLayout *L, LBlock *b, float cb_w, float avail_margin_w, int mode);
float l_clamp_w(const LBlock *b, float w, float cb_w);
float l_clamp_h(const LBlock *b, float h, float cb_h);
float l_spec_height(const LBlock *b, float cb_h); /* content-box height or -1 for auto */
float l_spec_width(const LBlock *b, float cb_w);  /* content-box width or -1 for auto */
void l_layout_independent(AuLayout *L, LBlock *b, float cb_w, float cb_h, float avail_w, int mode);
void l_layout_content(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h);
/* The border-box height l_layout_content would give, from a one-entry
 * cache when it can (the box's geometry is then not updated: use it only
 * where a later l_layout_content call does the final layout). */
float l_measure_content(AuLayout *L, LBlock *b, float content_w, float content_h, float cb_h);
void l_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c);
void l_outer_intrinsic(AuLayout *L, LBlock *b, float cb_w, float *min_o, float *max_o);
float l_edges_h(const LBlock *b); /* horizontal margins + borders + padding */
float l_bp_h(const LBlock *b);    /* horizontal borders + padding */
float l_bp_v(const LBlock *b);
void l_finish_overflow(LBlock *b);
void l_queue_abspos(AuLayout *L, LBlock *b, float sx, float sy);
void l_layout_abspos_for(AuLayout *L, LBox *cb, uint32_t mark);
void l_apply_relative(LBlock *b, float cb_w, float cb_h);
LBlock *l_geom_parent(LBox *b);
void l_box_origin(LBox *b, float *x, float *y); /* border-box origin in document coordinates (no scroll) */
bool l_aspect_ratio(const LBlock *b, float *ratio);
float l_ratio_height(const LBlock *b, float content_w);
/* The baseline of atomic inline k from its margin-box top. */
float l_atomic_baseline(const LBlock *k);
void l_shift_content(LBlock *b, float dy);
/* Form controls: the inner line height (and the baseline in it), list box
 * row height, select arrow width, file input button width. */
float l_control_line(AuLayout *L, const LBlock *b, float *ascent);
float l_listbox_row(AuLayout *L, const LBlock *b);
float l_select_arrow_w(const LBlock *b);
float l_file_button_w(AuLayout *L, const LBlock *b);

/* block.c */
float l_flow_layout(AuLayout *L, LBlock *b, const LConstraints *c);
void l_flow_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c);
void l_flow_after(AuLayout *L, LBlock *b); /* list markers of a laid-out item */

/* inline.c. flow is the enclosing block formatting context's state when
 * the IFC root takes part in it (margins collapse through it until its
 * first line), or NULL. l_flow_resolve places pending margins and returns
 * the IFC root's content top in BFC coordinates. */
typedef struct LBfc LBfc;
typedef struct LFlowRef {
    LBfc *S;
    int level;
} LFlowRef;
float l_flow_resolve(LFlowRef *fr);
float l_inline_layout(AuLayout *L, LBlock *b, float content_w, LFloats *fl, float bfc_x, float bfc_y, float cb_h,
                      LFlowRef *flow);
void l_inline_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c);

/* floats (block.c) */
struct LFloats {
    struct LFloatRec {
        float x, y, w, h; /* margin box, in the BFC root's content coordinates */
        uint8_t side;     /* 0 left, 1 right */
    } *f;
    uint32_t n, cap;
};
void l_floats_free(LFloats *fl);
void l_floats_avail(const LFloats *fl, float y, float h, float left, float right, float *l, float *r);
float l_floats_next_bottom(const LFloats *fl, float y);
float l_floats_clear_y(const LFloats *fl, int clear);
void l_place_float(AuLayout *L, LFloats *fl, LBlock *f, float cb_w, float cb_h, float left, float right,
                   float y, float bfc_x, float bfc_y);
float l_floats_bottom(const LFloats *fl);

/* flex.c */
float l_flex_layout(AuLayout *L, LBlock *b, const LConstraints *c);
void l_flex_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c);

/* table.c */
float l_table_layout(AuLayout *L, LBlock *b, const LConstraints *c);
void l_table_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c);

/* stack.c / draw.c / hit.c */
void l_build_stacking(AuLayout *L);
void l_paint(AuLayout *L, AuCanvas *c, LArea dirty);
bool l_hit(AuLayout *L, float x, float y, AuHitResult *r);
void l_dump(AuLayout *L, AuBuf *out);
float l_sticky_offset(AuLayout *L, LBlock *b, float *dx);

/* Small helpers */
static inline float l_max(float a, float b) { return a > b ? a : b; }
static inline float l_min(float a, float b) { return a < b ? a : b; }
static inline float l_len(const AuLen *l, float basis) { return au_len_resolve(l, basis < 0 ? 0 : basis); }
static inline bool l_len_pct(const AuLen *l) { return l->kind == AU_LEN_PCT || l->kind == AU_LEN_CALC; }

#endif
