/* Aura CSS: style sheet parsing, selector matching, the cascade and
 * computed values.
 *
 * To style a document:
 *
 *     AuCssResolver *r = au_css_resolver_new();      // has the UA sheet
 *     AuCssDevice dev; au_css_device_default(&dev);
 *     dev.width = 800; dev.height = 600;
 *     au_css_resolver_set_device(r, &dev);
 *     AuCssSheet *s = au_css_sheet_parse(text, len, AU_CSS_ORIGIN_AUTHOR,
 *                                        base_url, NULL);
 *     au_css_resolver_add_sheet(r, s);               // r owns s now
 *     au_css_style_document(r, doc);                 // fills el->style
 *     const AuStyle *st = au_element_style(el);
 *     float w = st->box->width.u.px;                 // if kind == AU_LEN_PX
 *
 * Computed styles are reference counted and immutable once computed. The
 * style of an element is split into groups (inherited common, inherited
 * rare, box, surround, background, flex/grid, rare); identical groups are
 * shared between elements, and elements that match the same rules under
 * the same parent share one AuStyle.
 *
 * Lengths in computed values are absolute CSS px, except percentages, which
 * stay for layout to resolve (AuLen, au_len_resolve). Colors are 32-bit
 * RGBA; currentcolor is kept as AU_COLOR_CURRENT and resolved with
 * au_style_color. Complex values (backgrounds, shadows, transforms, grid
 * templates, content) are value trees (AuCssBlob) with the same absolute
 * units.
 *
 * Engine rules: no OS calls, allocation through au_alloc, and no recursion
 * proportional to document depth. */
#ifndef AURA_CSS_H
#define AURA_CSS_H

#include "../dom/dom.h"
#include "css_enums.h"

/* ---- Keywords ----
 * Every CSS keyword the engine knows has a dense id. Value trees carry
 * these ids; au_css_keyword_atom maps one to its atom. */
typedef enum {
    AU_K_NONE_ = 0,
#define KW(id, str) AU_K_##id,
#include "keywords.def"
#undef KW
    AU_K_COUNT
} AuCssKeyword;

/* ---- Properties ----
 * Longhands first (AU_PROP_color ... ), then flow-relative longhands, then
 * shorthands. AU_PROP_CUSTOM stands for any custom property (--name). */
typedef enum {
    AU_PROP_NONE_ = 0,
#define P(id, name, type, flags, group, field, extra, initial) AU_PROP_##id,
#include "props.def"
    AU_PROP_LONGHAND_END,
    AU_PROP_LOGICAL_BEFORE_ = AU_PROP_LONGHAND_END - 1,
#define L(id, name, lgroup, side) AU_PROP_##id,
#include "props.def"
    AU_PROP_LOGICAL_END,
    AU_PROP_SHORTHAND_BEFORE_ = AU_PROP_LOGICAL_END - 1,
#define S(id, name, expander) AU_PROP_##id,
#include "props.def"
    AU_PROP_COUNT,
    AU_PROP_CUSTOM = AU_PROP_COUNT
} AuCssProp;

/* ---- Keyword enums of computed values ---- */
#define AU_KWL_ENUM_(list, e, k) AU_##list##_##e,
#define AU_KWL_DECLARE_(list) enum { AU_KWL_##list(AU_KWL_ENUM_) AU_##list##_COUNT_ };
AU_KWL_ALL(AU_KWL_DECLARE_)
#undef AU_KWL_DECLARE_

/* Display, as the two-value model: outer type, inner type, list-item flag.
 * Internal table and ruby types and none/contents are inner values. */
enum { AU_DISPLAY_OUTER_BLOCK = 0, AU_DISPLAY_OUTER_INLINE = 1, AU_DISPLAY_OUTER_RUN_IN = 2 };
enum {
    AU_DISPLAY_INNER_FLOW = 0,
    AU_DISPLAY_INNER_FLOW_ROOT,
    AU_DISPLAY_INNER_TABLE,
    AU_DISPLAY_INNER_FLEX,
    AU_DISPLAY_INNER_GRID,
    AU_DISPLAY_INNER_RUBY,
    AU_DISPLAY_INNER_WEBKIT_BOX, /* -webkit-box: legacy flex box, used with -webkit-line-clamp */
    AU_DISPLAY_INNER_TABLE_ROW_GROUP,
    AU_DISPLAY_INNER_TABLE_HEADER_GROUP,
    AU_DISPLAY_INNER_TABLE_FOOTER_GROUP,
    AU_DISPLAY_INNER_TABLE_ROW,
    AU_DISPLAY_INNER_TABLE_CELL,
    AU_DISPLAY_INNER_TABLE_COLUMN_GROUP,
    AU_DISPLAY_INNER_TABLE_COLUMN,
    AU_DISPLAY_INNER_TABLE_CAPTION,
    AU_DISPLAY_INNER_RUBY_BASE,
    AU_DISPLAY_INNER_RUBY_TEXT,
    AU_DISPLAY_INNER_RUBY_BASE_CONTAINER,
    AU_DISPLAY_INNER_RUBY_TEXT_CONTAINER,
    AU_DISPLAY_INNER_CONTENTS,
    AU_DISPLAY_INNER_NONE
};
#define AU_DISPLAY_MAKE(outer, inner, li) ((uint16_t)((outer) | ((inner) << 2) | ((li) ? 0x80 : 0)))
static inline int au_display_outer(uint16_t d) { return d & 3; }
static inline int au_display_inner(uint16_t d) { return (d >> 2) & 0x1F; }
static inline bool au_display_list_item(uint16_t d) { return (d & 0x80) != 0; }
#define AU_DISPLAY_NONE AU_DISPLAY_MAKE(0, AU_DISPLAY_INNER_NONE, 0)
#define AU_DISPLAY_CONTENTS AU_DISPLAY_MAKE(0, AU_DISPLAY_INNER_CONTENTS, 0)
#define AU_DISPLAY_BLOCK AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_FLOW, 0)
#define AU_DISPLAY_INLINE AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_FLOW, 0)
#define AU_DISPLAY_INLINE_BLOCK AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_FLOW_ROOT, 0)
#define AU_DISPLAY_FLOW_ROOT AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_FLOW_ROOT, 0)
#define AU_DISPLAY_LIST_ITEM AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_FLOW, 1)
#define AU_DISPLAY_FLEX AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_FLEX, 0)
#define AU_DISPLAY_INLINE_FLEX AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_FLEX, 0)
#define AU_DISPLAY_GRID AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_GRID, 0)
#define AU_DISPLAY_INLINE_GRID AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_GRID, 0)
#define AU_DISPLAY_TABLE AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_BLOCK, AU_DISPLAY_INNER_TABLE, 0)
#define AU_DISPLAY_INLINE_TABLE AU_DISPLAY_MAKE(AU_DISPLAY_OUTER_INLINE, AU_DISPLAY_INNER_TABLE, 0)

/* Alignment values (justify-*, align-*), in the low byte of a uint16_t;
 * AU_ALIGN_SAFE, AU_ALIGN_UNSAFE and AU_ALIGN_LEGACY are flags above it.
 * AU_ALIGN_LAST_BASELINE is "last baseline"; "first baseline" is
 * AU_ALIGN_BASELINE. */
enum {
    AU_ALIGN_AUTO = 0,
    AU_ALIGN_NORMAL,
    AU_ALIGN_STRETCH,
    AU_ALIGN_BASELINE,
    AU_ALIGN_LAST_BASELINE,
    AU_ALIGN_CENTER,
    AU_ALIGN_START,
    AU_ALIGN_END,
    AU_ALIGN_FLEX_START,
    AU_ALIGN_FLEX_END,
    AU_ALIGN_SELF_START,
    AU_ALIGN_SELF_END,
    AU_ALIGN_LEFT,
    AU_ALIGN_RIGHT,
    AU_ALIGN_SPACE_BETWEEN,
    AU_ALIGN_SPACE_AROUND,
    AU_ALIGN_SPACE_EVENLY,
    AU_ALIGN_LEGACY_ONLY, /* justify-items: legacy with no position */
    AU_ALIGN_SAFE = 0x100,
    AU_ALIGN_UNSAFE = 0x200,
    AU_ALIGN_LEGACY = 0x400
};

/* vertical-align keywords; AU_VALIGN_LENGTH uses vertical_align_len. */
enum {
    AU_VALIGN_BASELINE = 0,
    AU_VALIGN_SUB,
    AU_VALIGN_SUPER,
    AU_VALIGN_TEXT_TOP,
    AU_VALIGN_TEXT_BOTTOM,
    AU_VALIGN_MIDDLE,
    AU_VALIGN_TOP,
    AU_VALIGN_BOTTOM,
    AU_VALIGN_LENGTH
};

/* text-decoration-line bits. */
enum { AU_TDL_UNDERLINE = 1, AU_TDL_OVERLINE = 2, AU_TDL_LINE_THROUGH = 4, AU_TDL_BLINK = 8,
       AU_TDL_SPELLING_ERROR = 16, AU_TDL_GRAMMAR_ERROR = 32 };
/* contain bits. */
enum { AU_CONTAIN_SIZE = 1, AU_CONTAIN_INLINE_SIZE = 2, AU_CONTAIN_LAYOUT = 4, AU_CONTAIN_STYLE = 8,
       AU_CONTAIN_PAINT = 16 };
/* grid-auto-flow bits. */
enum { AU_GRID_FLOW_ROW = 0, AU_GRID_FLOW_COLUMN = 1, AU_GRID_FLOW_DENSE = 2 };
/* z-index and -webkit-line-clamp keyword values. */
#define AU_INT_AUTO INT32_MIN
#define AU_INT_NONE INT32_MIN

/* ---- Units ---- */
typedef enum {
    AU_UNIT_NONE = 0,
    /* absolute lengths */
    AU_UNIT_PX, AU_UNIT_CM, AU_UNIT_MM, AU_UNIT_Q, AU_UNIT_IN, AU_UNIT_PT, AU_UNIT_PC,
    /* font-relative */
    AU_UNIT_EM, AU_UNIT_REM, AU_UNIT_EX, AU_UNIT_REX, AU_UNIT_CH, AU_UNIT_RCH, AU_UNIT_CAP, AU_UNIT_RCAP,
    AU_UNIT_IC, AU_UNIT_RIC, AU_UNIT_LH, AU_UNIT_RLH,
    /* viewport (the s/l/d variants and container units are equal here) */
    AU_UNIT_VW, AU_UNIT_VH, AU_UNIT_VI, AU_UNIT_VB, AU_UNIT_VMIN, AU_UNIT_VMAX,
    AU_UNIT_SVW, AU_UNIT_SVH, AU_UNIT_SVI, AU_UNIT_SVB, AU_UNIT_SVMIN, AU_UNIT_SVMAX,
    AU_UNIT_LVW, AU_UNIT_LVH, AU_UNIT_LVI, AU_UNIT_LVB, AU_UNIT_LVMIN, AU_UNIT_LVMAX,
    AU_UNIT_DVW, AU_UNIT_DVH, AU_UNIT_DVI, AU_UNIT_DVB, AU_UNIT_DVMIN, AU_UNIT_DVMAX,
    AU_UNIT_CQW, AU_UNIT_CQH, AU_UNIT_CQI, AU_UNIT_CQB, AU_UNIT_CQMIN, AU_UNIT_CQMAX,
    AU_UNIT_LENGTH_END_,
    /* other dimensions */
    AU_UNIT_DEG = AU_UNIT_LENGTH_END_, AU_UNIT_GRAD, AU_UNIT_RAD, AU_UNIT_TURN,
    AU_UNIT_S, AU_UNIT_MS, AU_UNIT_HZ, AU_UNIT_KHZ,
    AU_UNIT_DPI, AU_UNIT_DPCM, AU_UNIT_DPPX, AU_UNIT_X,
    AU_UNIT_FR,
    AU_UNIT_UNKNOWN,
    AU_UNIT_COUNT
} AuCssUnit;

static inline bool au_unit_is_length(int u) { return u >= AU_UNIT_PX && u < AU_UNIT_LENGTH_END_; }

/* ---- Value trees ----
 * Declared values (in style sheets) and complex computed values are trees
 * of AuCssValue nodes. A tree is stored in one reference-counted block,
 * AuCssBlob. In computed trees, lengths are AU_UNIT_PX dimensions, angles
 * deg, times s, and colors resolved (AU_COLOR_CURRENT aside). */
typedef enum {
    AU_CSSV_INVALID = 0,
    AU_CSSV_KEYWORD,    /* kw */
    AU_CSSV_IDENT,      /* u.str: a custom identifier (font names, counter names, grid line names) */
    AU_CSSV_STRING,     /* u.str */
    AU_CSSV_URL,        /* u.str: as written; resolve against AuCssBlob.base_url */
    AU_CSSV_NUMBER,     /* u.num; unit 1 if written as an integer */
    AU_CSSV_PERCENT,    /* u.num */
    AU_CSSV_DIMENSION,  /* u.num, unit */
    AU_CSSV_COLOR,      /* u.rgba */
    AU_CSSV_FUNCTION,   /* kw = function name; u.items = arguments, one per comma-separated group */
    AU_CSSV_CALC,       /* unit = AuCssCalcOp; u.items = operands */
    AU_CSSV_LIST,       /* space-separated; u.items */
    AU_CSSV_COMMA_LIST, /* comma-separated; u.items */
    AU_CSSV_DELIM       /* unit = the character ('/' in lists) */
} AuCssValueType;

typedef enum {
    AU_CALC_SUM, AU_CALC_PRODUCT, AU_CALC_NEGATE, AU_CALC_INVERT,
    AU_CALC_MIN, AU_CALC_MAX, AU_CALC_CLAMP,
    AU_CALC_ROUND_NEAREST, AU_CALC_ROUND_UP, AU_CALC_ROUND_DOWN, AU_CALC_ROUND_TO_ZERO,
    AU_CALC_MOD, AU_CALC_REM, AU_CALC_ABS, AU_CALC_SIGN,
    AU_CALC_SIN, AU_CALC_COS, AU_CALC_TAN, AU_CALC_ASIN, AU_CALC_ACOS, AU_CALC_ATAN, AU_CALC_ATAN2,
    AU_CALC_POW, AU_CALC_SQRT, AU_CALC_HYPOT, AU_CALC_LOG, AU_CALC_EXP
} AuCssCalcOp;

typedef struct AuCssValue AuCssValue;
struct AuCssValue {
    uint8_t type; /* AuCssValueType */
    uint8_t unit; /* AuCssUnit, AuCssCalcOp, delimiter, or integer flag */
    uint16_t kw;  /* AuCssKeyword for KEYWORD and FUNCTION */
    uint32_t n;   /* item count, or string length */
    union {
        float num;
        uint32_t rgba;
        const char *str; /* NUL-terminated */
        AuCssValue *items;
    } u;
};

typedef struct AuCssBlob {
    uint32_t refs;
    uint32_t size;        /* bytes, including this header */
    const char *base_url; /* for url() values; may be NULL */
    AuCssValue value;
} AuCssBlob;

AuCssBlob *au_css_blob_retain(AuCssBlob *b);
void au_css_blob_release(AuCssBlob *b);

/* ---- Colors ----
 * RGBA, red in the high byte. AU_COLOR_CURRENT stands for currentcolor;
 * a color that happens to have the same bits is stored as transparent. */
typedef uint32_t AuColor;
#define AU_COLOR_RGBA(r, g, b, a) \
    ((AuColor)(((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a)))
#define AU_COLOR_CURRENT ((AuColor)0x01020300u)
#define AU_COLOR_TRANSPARENT ((AuColor)0)

/* ---- Lengths ----
 * A computed length-percentage. kind says which member is meaningful. */
typedef enum {
    AU_LEN_PX = 0,      /* u.px */
    AU_LEN_PCT,         /* u.pct, percent (50 for 50%) */
    AU_LEN_CALC,        /* u.calc: a px/percent expression; use au_len_resolve */
    AU_LEN_NUMBER,      /* u.num: line-height and tab-size numbers */
    AU_LEN_AUTO,
    AU_LEN_NONE,
    AU_LEN_NORMAL,
    AU_LEN_MIN_CONTENT,
    AU_LEN_MAX_CONTENT,
    AU_LEN_FIT_CONTENT,
    AU_LEN_FIT_CONTENT_PX, /* fit-content(<length>), u.px */
    AU_LEN_STRETCH,     /* stretch, -webkit-fill-available */
    AU_LEN_CONTENT,     /* flex-basis: content */
    AU_LEN_FROM_FONT
} AuLenKind;

typedef struct AuLen {
    union {
        float px;
        float pct;
        float num;
        AuCssBlob *calc;
    } u;
    uint8_t kind; /* AuLenKind */
} AuLen;

/* Resolves px, percent and calc lengths against a percentage basis (px).
 * Other kinds give 0. */
float au_len_resolve(const AuLen *l, float basis);
static inline bool au_len_is_auto(const AuLen *l) { return l->kind == AU_LEN_AUTO; }
/* True for px, pct and calc. */
static inline bool au_len_is_definite_kind(const AuLen *l) { return l->kind <= AU_LEN_CALC; }

/* ---- Computed style groups ----
 * Each group is reference counted and shared until written. Fields marked
 * blob hold a computed value tree or NULL for the initial "none"/"auto"
 * when noted. */
typedef struct AuCustomProps AuCustomProps;

typedef struct AuStyleInh { /* inherited, common */
    uint32_t refs;
    AuColor color;
    float font_size;    /* px: the used size, after the minimum font size */
    float font_size_spec; /* px before the minimum: what relative sizes of children start from */
    float font_stretch; /* percent */
    AuLen line_height;  /* NORMAL, NUMBER or PX */
    AuCssBlob *font_family;     /* COMMA_LIST of STRING/IDENT/KEYWORD (generic families) */
    AuCssBlob *list_style_type; /* KEYWORD (counter style name or none), IDENT or STRING */
    AuCustomProps *custom;      /* custom properties, NULL if none */
    uint16_t font_weight;       /* 1..1000 */
    uint8_t font_style;         /* AU_FONT_STYLE_* */
    uint8_t font_variant_caps;
    uint8_t text_align;
    uint8_t white_space_collapse;
    uint8_t text_wrap_mode;
    uint8_t direction;
    uint8_t writing_mode;
    uint8_t visibility;
    uint8_t list_style_position;
    uint8_t font_size_kw; /* 1..8 for xx-small..xxx-large when the size comes from a keyword, else 0 */
} AuStyleInh;

typedef struct AuStyleInhRare { /* inherited, rarely set */
    uint32_t refs;
    float letter_spacing; /* px; normal is 0 */
    float word_spacing;
    float border_spacing[2]; /* horizontal, vertical px */
    AuLen text_indent;
    AuLen tab_size;          /* NUMBER (spaces) or PX */
    AuLen text_size_adjust;  /* AUTO, NONE or PCT */
    AuColor text_fill_color; /* -webkit-text-fill-color */
    AuCssBlob *quotes;       /* KEYWORD auto/none or LIST of STRING */
    AuCssBlob *text_shadow;  /* NULL for none; COMMA_LIST of LIST(x y blur color) */
    AuCssBlob *list_style_image; /* NULL for none */
    AuCssBlob *cursor_images;    /* NULL or COMMA_LIST of LIST(url [x y]) */
    /* NULL for normal; COMMA_LIST of LIST(STRING tag, NUMBER value) */
    AuCssBlob *font_variation_settings;
    /* SVG paint: NULL for none; COLOR (rgba AU_COLOR_CURRENT for
     * currentcolor), URL, LIST(URL, fallback COLOR or none keyword), or
     * the keyword context-fill / context-stroke. */
    AuCssBlob *fill, *stroke;
    AuCssBlob *stroke_dasharray; /* NULL for none; COMMA_LIST or LIST of px DIMENSION, NUMBER, PERCENT */
    AuCssBlob *paint_order;      /* KEYWORD normal, or LIST of fill / stroke / markers keywords */
    AuLen stroke_width, stroke_dashoffset; /* PX, PCT, CALC or NUMBER (user units) */
    float fill_opacity, stroke_opacity, stroke_miterlimit;
    uint8_t text_transform, text_align_last, text_wrap_style, word_break, overflow_wrap, hyphens,
        line_break, border_collapse, caption_side, empty_cells, pointer_events, cursor, font_kerning,
        text_rendering, image_rendering, font_optical_sizing, fill_rule, clip_rule, stroke_linecap,
        stroke_linejoin, text_anchor;
} AuStyleInhRare;

typedef struct AuStyleBox {
    uint32_t refs;
    AuLen width, height, min_width, min_height, max_width, max_height;
    AuLen top, right, bottom, left;
    AuLen vertical_align_len;
    float aspect_ratio; /* width / height, 0 if none */
    int32_t z_index;    /* AU_INT_AUTO for auto */
    uint16_t display;   /* AU_DISPLAY_*; blockified where CSS requires */
    uint16_t display_specified; /* before blockification */
    uint8_t position, float_, clear, box_sizing, overflow_x, overflow_y, vertical_align,
        aspect_ratio_auto, unicode_bidi, table_layout;
} AuStyleBox;

typedef struct AuStyleSurround {
    uint32_t refs;
    AuLen margin[4];  /* top, right, bottom, left */
    AuLen padding[4];
    float border_width[4]; /* px, 0 when the style is none or hidden */
    AuColor border_color[4];
    AuLen radius[4][2];    /* top-left, top-right, bottom-right, bottom-left; horizontal, vertical */
    uint8_t border_style[4];
} AuStyleSurround;

typedef struct AuStyleBackground {
    uint32_t refs;
    AuColor background_color;
    /* Each a COMMA_LIST with one item per layer as written; the layer
     * count is that of background_image (au_style_bg_layer). */
    AuCssBlob *background_image, *background_position_x, *background_position_y, *background_size,
        *background_repeat, *background_attachment, *background_clip, *background_origin;
} AuStyleBackground;

typedef struct AuStyleFlex {
    uint32_t refs;
    float flex_grow, flex_shrink;
    AuLen flex_basis;
    AuLen row_gap, column_gap; /* NORMAL or a length */
    int32_t order;
    AuCssBlob *grid_template_columns, *grid_template_rows, *grid_template_areas;
    AuCssBlob *grid_auto_columns, *grid_auto_rows;
    AuCssBlob *grid_row_start, *grid_row_end, *grid_column_start, *grid_column_end;
    uint16_t justify_content, align_content, justify_items, align_items, justify_self, align_self;
    uint8_t flex_direction, flex_wrap, grid_auto_flow;
} AuStyleFlex;

typedef struct AuStyleRare {
    uint32_t refs;
    float opacity;
    float outline_width, outline_offset;
    AuColor outline_color, text_decoration_color;
    AuLen perspective, text_decoration_thickness;
    int32_t line_clamp; /* -webkit-line-clamp, AU_INT_NONE for none */
    AuCssBlob *transform, *transform_origin, *translate, *rotate, *scale, *perspective_origin;
    AuCssBlob *transition_property, *transition_duration, *transition_timing_function, *transition_delay,
        *transition_behavior;
    AuCssBlob *animation_name, *animation_duration, *animation_timing_function, *animation_delay,
        *animation_iteration_count, *animation_direction, *animation_fill_mode, *animation_play_state;
    AuCssBlob *filter, *backdrop_filter, *box_shadow, *content, *counter_reset, *counter_increment,
        *counter_set, *object_position, *will_change, *clip_path, *clip;
    /* CSS masks: COMMA_LISTs with one item per layer, like the background
     * lists (au_style_mask_layer). */
    AuCssBlob *mask_image, *mask_mode, *mask_position_x, *mask_position_y, *mask_size, *mask_repeat,
        *mask_origin, *mask_clip;
    AuCssBlob *container_name; /* NULL for none; IDENT or LIST of IDENT */
    AuColor stop_color;
    float stop_opacity;
    /* contain-intrinsic-width and -height in px: the size of a box with
     * size containment (none is 0; auto <length> takes the length). */
    float contain_intrinsic_w, contain_intrinsic_h;
    uint8_t outline_style, transform_style, backface_visibility, object_fit, resize, appearance,
        scroll_behavior, scrollbar_width, contain, isolation, mix_blend_mode, text_decoration_line, text_decoration_style,
        text_overflow, user_select, box_orient, vector_effect, mask_type, dominant_baseline,
        container_type; /* AU_CONTAINER_TYPE_* */
} AuStyleRare;

typedef enum {
    AU_CSS_PSEUDO_NONE = 0,
    AU_CSS_PSEUDO_BEFORE,
    AU_CSS_PSEUDO_AFTER,
    AU_CSS_PSEUDO_MARKER,
    AU_CSS_PSEUDO_PLACEHOLDER,
    AU_CSS_PSEUDO_SELECTION,
    AU_CSS_PSEUDO_FIRST_LINE,
    AU_CSS_PSEUDO_FIRST_LETTER,
    AU_CSS_PSEUDO_BACKDROP,
    AU_CSS_PSEUDO_FILE_SELECTOR_BUTTON,
    AU_CSS_PSEUDO_UNKNOWN_WEBKIT, /* ::-webkit-*: valid, never matches */
    AU_CSS_PSEUDO_SLOTTED,        /* ::slotted(): styles the slot's assigned elements */
    AU_CSS_PSEUDO_PART,           /* ::part(): styles elements of the host's shadow tree */
    AU_CSS_PSEUDO_COUNT
} AuCssPseudo;

typedef struct AuStyle AuStyle;
struct AuStyle {
    uint32_t refs;
    uint8_t pseudo; /* AuCssPseudo of this style */
    uint8_t flags;  /* AU_STYLE_F_* */
    uint16_t pseudo_bits; /* 1 << AuCssPseudo for each pseudo-element some rule matched */
    AuStyleInh *inh;
    AuStyleInhRare *inhr;
    AuStyleBox *box;
    AuStyleSurround *surr;
    AuStyleBackground *bg;
    AuStyleFlex *flex;
    AuStyleRare *rare;
    AuStyle *pseudos; /* computed ::before, ::after, ::marker styles of this element, linked by next */
    AuStyle *next;
};
enum {
    AU_STYLE_F_VIEWPORT_UNITS = 1, /* depends on the viewport size */
    AU_STYLE_F_HAS_ATTR = 2,       /* uses attr() */
    AU_STYLE_F_ROOT = 4,
    AU_STYLE_F_AFFECTED_BY_HAS = 8,
    /* A form control (input, select, textarea, button, progress, meter)
     * whose background, border or box-shadow comes from author styles:
     * layout paints it as a plain CSS box instead of a native-looking
     * control, as browsers do for appearance: auto. */
    AU_STYLE_F_AUTHOR_DECOR = 16,
    /* Uses a container unit resolved against a container's size: not
     * shared with an element that has the same parent style. */
    AU_STYLE_F_CONTAINER_UNITS = 32
};

AuStyle *au_style_retain(AuStyle *s);
void au_style_release(AuStyle *s);
/* The style AuElement.style holds, or NULL. */
static inline const AuStyle *au_element_style(const AuElement *el) { return (const AuStyle *)el->style; }
/* A pseudo-element style computed by au_css_style_document, or NULL. */
const AuStyle *au_style_pseudo(const AuStyle *s, AuCssPseudo p);
/* Resolves AU_COLOR_CURRENT against the style's color. */
static inline AuColor au_style_color(const AuStyle *s, AuColor c) { return c == AU_COLOR_CURRENT ? s->inh->color : c; }

/* One background layer, with the shorter lists repeated as CSS requires.
 * Values are nodes inside the style's blobs. */
typedef struct AuBgLayer {
    const AuCssValue *image;      /* KEYWORD none, URL or FUNCTION (gradients) */
    const AuCssValue *position_x; /* PERCENT, DIMENSION px or CALC */
    const AuCssValue *position_y;
    const AuCssValue *size;       /* KEYWORD cover/contain, or LIST of two (auto keyword or length) */
    uint16_t repeat_x, repeat_y;  /* AU_K_repeat, AU_K_no_repeat, AU_K_space, AU_K_round */
    uint16_t attachment, clip, origin; /* keywords */
    const char *base_url;              /* resolve a url() image against it; NULL: the document */
} AuBgLayer;
size_t au_style_bg_layer_count(const AuStyle *s);
void au_style_bg_layer(const AuStyle *s, size_t i, AuBgLayer *out);

/* One mask layer (mask-image and its lists), in the background layer's
 * shape: attachment holds the mask-mode keyword (alpha, luminance or
 * match-source). The count is 0 when mask-image is none for every layer. */
size_t au_style_mask_layer_count(const AuStyle *s);
void au_style_mask_layer(const AuStyle *s, size_t i, AuBgLayer *out);

/* ---- Custom properties ---- */
/* Returns the computed (substituted) value text of --name, or NULL. */
const char *au_style_custom_property(const AuStyle *s, AuAtom name, size_t *len);

/* ---- Generic computed value access ----
 * Appends the serialized computed value of a property (longhand,
 * shorthand or custom, by atom of its name) and returns false if the
 * property is unknown. For getComputedStyle. */
bool au_style_serialize_property(const AuStyle *s, AuAtom name, AuBuf *out);
/* The id of a property name (lowercase atom), or AU_PROP_NONE_. Aliases
 * map to their target. */
AuCssProp au_css_property_id(AuAtom name);
const char *au_css_property_name(AuCssProp p);
AuAtom au_css_keyword_atom(AuCssKeyword k);
const char *au_css_keyword_name(AuCssKeyword k);
/* The keyword id of a lowercase atom, or AU_K_NONE_. */
AuCssKeyword au_css_keyword_of(AuAtom a);

/* Serializes a value tree. */
void au_css_value_serialize(const AuCssValue *v, AuBuf *out);
void au_css_color_serialize(AuColor c, AuBuf *out);

/* ---- Style sheets ---- */
typedef enum { AU_CSS_ORIGIN_UA = 0, AU_CSS_ORIGIN_USER = 1, AU_CSS_ORIGIN_AUTHOR = 2 } AuCssOrigin;

typedef struct AuCssSheet AuCssSheet;
typedef struct AuCssDeclBlock AuCssDeclBlock;
typedef struct AuCssMediaList AuCssMediaList;

/* Parses a style sheet. base_url may be NULL. media is the media
 * attribute of the link or style element (NULL or "" for all). Never fails:
 * invalid parts are dropped as CSS requires. */
AuCssSheet *au_css_sheet_parse(const char *css, size_t len, AuCssOrigin origin, const char *base_url,
                               const char *media);
/* The same, taking css, a block from au_alloc: the sheet keeps it instead
 * of a copy and frees it. */
AuCssSheet *au_css_sheet_parse_take(char *css, size_t len, AuCssOrigin origin, const char *base_url,
                                    const char *media);
/* The sheet keeps the text it was parsed from: blocks of declarations
 * are parsed from it when a rule first matches. au_css_sheet_text_get
 * appends it to out (false: the sheet has none); au_css_sheet_text_is
 * says whether it is exactly these bytes. au_css_sheet_pack_text deflates
 * it in place (call it once the document has loaded: reading a block
 * then inflates a piece of the text); it returns true if it did, false
 * if the text is small, already packed or does not compress. */
bool au_css_sheet_text_get(const AuCssSheet *s, AuBuf *out);
bool au_css_sheet_text_is(const AuCssSheet *s, const char *text, size_t len);
bool au_css_sheet_pack_text(AuCssSheet *s);
/* How many blocks have been parsed late from sheet text, over all sheets:
 * while it grows, a document is still finding its rules, and packing its
 * sheets' texts would make each of those parses inflate a piece. */
uint32_t au_css_late_parse_count(void);
/* About what the sheet holds: its text and the chunks of its rules. */
size_t au_css_sheet_memory(const AuCssSheet *s);
/* A sheet is freed when its last owner frees it; au_css_sheet_retain adds
 * an owner (a document that links one sheet several times parses it
 * once). */
void au_css_sheet_free(AuCssSheet *s);
AuCssSheet *au_css_sheet_retain(AuCssSheet *s);
/* Parses css as au_css_sheet_parse_take does, or, when some owner still
 * holds a sheet parsed from the same bytes for the same base URL, media
 * and origin, frees css and returns that sheet with an owner added. The
 * documents of all tabs and frames share sheets this way. A shared sheet
 * must not be changed: to change a sheet's rules, parse a new one (the
 * CSSOM does). A sheet with @import rules is never shared, since its
 * owner attaches the imports. au_css_sheet_owners says how many owners a
 * sheet has; au_css_shared_sheet_count how many sheets can be shared. */
AuCssSheet *au_css_sheet_parse_shared(char *css, size_t len, AuCssOrigin origin, const char *base_url,
                                      const char *media);
uint32_t au_css_sheet_owners(const AuCssSheet *s);
size_t au_css_shared_sheet_count(void);
size_t au_css_sheet_rule_count(const AuCssSheet *s); /* style rules, nested ones included */

/* @import rules, in order. To load one, fetch url (relative to the
 * sheet's base URL), parse it and attach it with au_css_sheet_set_import;
 * the cascade then treats its rules as written at the @import. */
typedef struct AuCssImport {
    const char *url;
    const char *media;       /* media query text, "" for all */
    const char *layer;       /* layer name, "" for an anonymous layer, NULL for none */
    const char *supports;    /* supports() condition text, NULL for none */
    bool supported;          /* false if supports() was false: do not load */
    AuCssSheet *sheet;       /* attached sheet or NULL */
} AuCssImport;
size_t au_css_sheet_import_count(const AuCssSheet *s);
const AuCssImport *au_css_sheet_import(const AuCssSheet *s, size_t i);
/* Attaches a parsed sheet; s takes ownership. The resolver must be told
 * with au_css_resolver_sheets_changed. */
void au_css_sheet_set_import(AuCssSheet *s, size_t i, AuCssSheet *child);

/* @font-face rules. Descriptor values are generic value trees. */
typedef struct AuCssFontFace AuCssFontFace;
size_t au_css_sheet_font_face_count(const AuCssSheet *s);
const AuCssFontFace *au_css_sheet_font_face(const AuCssSheet *s, size_t i);
/* name: "font-family", "src", "font-weight", "font-style", "font-stretch",
 * "unicode-range", "font-display". Returns NULL if absent. */
const AuCssValue *au_css_font_face_descriptor(const AuCssFontFace *f, const char *name);
const char *au_css_font_face_base_url(const AuCssFontFace *f);

/* @keyframes. Offsets are 0..1. */
typedef struct AuCssKeyframe {
    float *offsets;
    uint32_t noffsets;
    AuCssDeclBlock *decls;
} AuCssKeyframe;
typedef struct AuCssKeyframes {
    AuAtom name;
    AuCssKeyframe *frames;
    uint32_t nframes;
} AuCssKeyframes;

/* ---- Values given as text (canvasval.c) ----
 * For the canvas context. A colour: any CSS <color> a declaration takes;
 * false when the text is not one. *current (optional) is set for
 * currentcolor, whose value the caller resolves. */
bool au_css_parse_color_text(const char *s, size_t len, AuColor *out, bool *current);
/* A font shorthand. base is the style relative sizes and weights refer to
 * (NULL: 10px, weight 400). The caller initializes and frees the two
 * buffers: families is a font-family value for au_fontlist_parse, text
 * the shorthand as it reads back. False (out unchanged) when the text is
 * not a font value; line-height is parsed and ignored. */
typedef struct AuCssCanvasFont {
    float size; /* px */
    int weight;
    bool italic, small_caps;
    AuBuf families, text;
} AuCssCanvasFont;
bool au_css_parse_font_text(const char *s, size_t len, const AuStyle *base, AuCssCanvasFont *out);

/* ---- Declaration blocks (inline style, CSSOM) ---- */
AuCssDeclBlock *au_css_parse_inline_style(const char *css, size_t len, const char *base_url);
void au_css_decl_block_free(AuCssDeclBlock *b);
/* Serializes as "prop: value; prop: value !important;". */
void au_css_decl_block_serialize(const AuCssDeclBlock *b, AuBuf *out);
/* Appends the declared value of one property (longhand or shorthand) and
 * returns false if not set (or, for a shorthand, not fully set). */
bool au_css_decl_block_get(const AuCssDeclBlock *b, AuAtom name, AuBuf *out);
size_t au_css_decl_block_count(const AuCssDeclBlock *b);

/* ---- Devices and media ---- */
typedef struct AuCssDevice {
    float width, height;               /* viewport, CSS px */
    float device_width, device_height; /* screen, CSS px */
    float resolution;                  /* device pixels per CSS px */
    uint8_t print;                     /* media type print instead of screen */
    uint8_t color_bits;                /* bits per color component, 0 for monochrome */
    uint8_t monochrome_bits;
    uint8_t dark;                      /* prefers-color-scheme: dark */
    uint8_t reduced_motion;            /* prefers-reduced-motion: reduce */
    uint8_t reduced_transparency;
    uint8_t contrast_more;
    uint8_t hover;                     /* primary pointer can hover */
    uint8_t pointer;                   /* 0 none, 1 coarse, 2 fine */
    uint8_t any_hover, any_pointer;
    uint8_t scripting;                 /* 0 none, 1 initial-only, 2 enabled */
    uint8_t forced_colors;
    uint8_t grid;
} AuCssDevice;

/* A desktop screen: 1024x768, 1 dppx, 8-bit color, mouse, light scheme. */
void au_css_device_default(AuCssDevice *d);

/* ---- Font metrics ----
 * ex and ch need font metrics, which belong to the font engine. The
 * resolver calls this with the font of the element (family, size, weight
 * and style are already computed in style->inh) and expects the x-height
 * and the advance of "0" in px. Without a callback both are 0.5em. */
typedef void (*AuCssMetricsFn)(void *ctx, const AuStyle *style, float *ex_px, float *ch_px);

/* ---- Font preferences ----
 * The user's font size settings, applied while computing font-size (they
 * follow Chrome's behaviour):
 * - default_size is the size of "medium". The other absolute keywords
 *   come from Chrome's table for a 16px default, else from its scale
 *   factors (xx-small 3/5, x-small 3/4, small 8/9, large 6/5, x-large
 *   3/2, xx-large 2, xxx-large 3).
 * - default_mono_size replaces default_size when font-family is exactly
 *   "monospace" (so <pre> and <code> get 13px). An element that changes
 *   between monospace and other families without setting font-size
 *   recomputes a keyword size from the other default, and scales any
 *   other inherited size by the ratio of the two defaults.
 * - text_zoom multiplies absolute font sizes (keywords and absolute
 *   lengths); relative sizes (em, %, larger) follow from their parent,
 *   so nothing is zoomed twice. Other properties are not zoomed.
 * - min_size clamps the computed font-size (what layout and em lengths
 *   use) but not font_size_spec, which children's relative sizes start
 *   from, so nesting does not compound the clamp. A size of 0 stays 0. */
typedef struct AuCssFontPrefs {
    float default_size;      /* px, 16 */
    float default_mono_size; /* px, 13 */
    float text_zoom;         /* 1 */
    float min_size;          /* px, 0 for none */
} AuCssFontPrefs;

void au_css_font_prefs_default(AuCssFontPrefs *p);

/* ---- The resolver ---- */
typedef struct AuCssResolver AuCssResolver;

AuCssResolver *au_css_resolver_new(void);
void au_css_resolver_free(AuCssResolver *r);
/* Sets the device. When only the viewport size differs from the current
 * device and the document has been styled, the resolver evaluates again
 * the media queries its rule index read: if none has another result, the
 * index is kept and au_css_restyle computes again only the elements whose
 * styles use viewport units (and what inherits from them), or nothing.
 * Any other difference restyles the document. */
void au_css_resolver_set_device(AuCssResolver *r, const AuCssDevice *d);
/* What the next au_css_restyle computes because of device changes. */
enum {
    AU_CSS_VIEWPORT_NONE = 0, /* nothing */
    AU_CSS_VIEWPORT_UNITS,    /* the elements whose styles use viewport units */
    AU_CSS_VIEWPORT_ALL       /* every element */
};
int au_css_resolver_viewport_restyle(const AuCssResolver *r);
const AuCssDevice *au_css_resolver_device(const AuCssResolver *r);
void au_css_resolver_set_metrics(AuCssResolver *r, AuCssMetricsFn fn, void *ctx);
/* ---- Container queries ----
 * Size queries (@container) and container units read the size of a query
 * container, which layout knows: fn sets *w and *h to the content-box
 * size of el's box in the last layout and returns false when el has none.
 * Without it, size queries are false and container units are small
 * viewport units. A style pass reads sizes from the last layout; after
 * each layout, au_css_resolver_containers_changed marks the containers
 * whose size is not the one read, and the caller restyles and lays out
 * again, a bounded number of times. */
typedef bool (*AuCssContainerSizeFn)(void *ctx, const AuElement *el, float *w, float *h);
void au_css_resolver_set_container_size(AuCssResolver *r, AuCssContainerSizeFn fn, void *ctx);
/* True when a style read some container's size. */
bool au_css_resolver_has_containers(const AuCssResolver *r);
/* Marks for restyle the flat subtree of each container (in doc) whose
 * size differs from the size read, and returns true if there was one:
 * then call au_css_restyle and lay out again. */
bool au_css_resolver_containers_changed(AuCssResolver *r, AuDocument *doc);
/* Drops the references the resolver holds to containers (call it before
 * the document goes). */
void au_css_resolver_forget_containers(AuCssResolver *r);
/* Sets the font preferences (copied) and restyles everything. A new
 * resolver uses au_css_font_prefs_default. Values out of range (sizes
 * not above 0, zoom not above 0) fall back to the defaults. */
void au_css_resolver_set_font_prefs(AuCssResolver *r, const AuCssFontPrefs *p);
const AuCssFontPrefs *au_css_resolver_font_prefs(const AuCssResolver *r);
/* Adds a user or author sheet after the existing ones; r owns it. */
void au_css_resolver_add_sheet(AuCssResolver *r, AuCssSheet *s);
/* The same for an author sheet of a tree scope: scope is the shadow root
 * whose <style>, <link> or adoptedStyleSheets gave it (NULL, or anything
 * else that is not a shadow root, for the document). Its rules apply
 * only to that shadow tree, its host (:host) and the elements slotted
 * into it (::slotted()). The caller keeps scope alive while the sheet is
 * added (r compares the pointer). A shadow root's sheets are given in
 * the order they apply: its own sheets in tree order, then its adopted
 * sheets. */
void au_css_resolver_add_scoped_sheet(AuCssResolver *r, AuCssSheet *s, const AuNode *scope);
/* Removes and frees a sheet. */
void au_css_resolver_remove_sheet(AuCssResolver *r, AuCssSheet *s);
/* Removes every added sheet without freeing it: the caller owns them
 * again. To keep parsed sheets across changes of the sheet list, detach,
 * then add the current ones in order. Detach before freeing the resolver
 * when the sheets must outlive it. */
void au_css_resolver_detach_sheets(AuCssResolver *r);
/* Call after attaching imports or otherwise changing sheets. */
void au_css_resolver_sheets_changed(AuCssResolver *r);
/* After the sheets were given again (detach, add) when only the sheets
 * of the shadow roots in roots changed: the next au_css_restyle builds the
 * index again and restyles the hosts' flat subtrees, not the document.
 * A root that is not a shadow root with a host makes it a full restyle,
 * as does anything that asked for one. */
void au_css_resolver_sheets_changed_in(AuCssResolver *r, AuNode *const *roots, size_t n);
/* Finds @keyframes by name across the author sheets (last one wins). */
const AuCssKeyframes *au_css_resolver_keyframes(AuCssResolver *r, AuAtom name);
/* True if the media query list matches the current device (matchMedia). */
bool au_css_media_matches(AuCssResolver *r, const char *query, size_t len);
/* The same against a device, without a resolver: whether a sheet with
 * this media attribute applies now. */
bool au_css_media_text_matches(const char *query, size_t len, const AuCssDevice *dev);
/* True if the condition is supported (CSS.supports(condition)). */
bool au_css_supports(const char *cond, size_t len);
/* CSS.supports(property, value). */
bool au_css_supports_decl(const char *prop, size_t plen, const char *value, size_t vlen);

/* Computes the style of el. parent is the parent element's style (NULL
 * for the root). Returns a new reference. */
AuStyle *au_css_compute(AuCssResolver *r, AuElement *el, const AuStyle *parent);
/* Computes a pseudo-element style of el, whose own style is el_style.
 * Returns NULL if no rule applies, or for ::before/::after whose content
 * computes to none or normal. */
AuStyle *au_css_compute_pseudo(AuCssResolver *r, AuElement *el, const AuStyle *el_style, AuCssPseudo p);

/* Styles every element of doc, storing a reference in AuElement.style
 * (releasing the previous one) and the ::before, ::after and ::marker
 * styles in AuStyle.pseudos. Elements in display:none subtrees are styled
 * too. */
void au_css_style_document(AuCssResolver *r, AuDocument *doc);
/* Restyles what au_css_invalidate marked; styles everything if nothing
 * was styled before. */
void au_css_restyle(AuCssResolver *r, AuDocument *doc);
/* The number of elements styled since the resolver was created (for
 * tests and timings: how much a change restyled). */
uint32_t au_css_resolver_styled_count(const AuCssResolver *r);
/* Releases every AuElement.style in doc. */
void au_css_clear_document(AuDocument *doc);

/* ---- Invalidation ----
 * Tell the resolver what changed; au_css_restyle then recomputes what is
 * affected. Call it before or after the change, and au_css_restyle after
 * it.
 *
 * State, attribute, class, id and inline style changes are targeted: the
 * element is restyled, plus the descendants, following siblings and
 * :has() anchors that some selector reading that state or naming that
 * class, id or attribute can reach from it. A class, id or attribute no
 * selector names (and that no presentational hint, attr() or pseudo-class
 * reads) restyles nothing. An element whose new style has the same values
 * keeps its old AuStyle object, and its subtree is left alone unless a
 * selector reaches into it; so equal styles stay pointer-equal.
 *
 * Child insertions and removals reported with au_css_invalidate_children
 * are targeted too; AU_CSS_CHANGE_CHILDREN without the child restyles the
 * element's subtree and its following siblings' subtrees, or the whole
 * document when a :has() rule exists. */
typedef enum {
    AU_CSS_CHANGE_ATTR,     /* an attribute, class or id changed (attr: its name) */
    AU_CSS_CHANGE_STATE,    /* hover, focus, checked, ... (AU_STATE_* bits) */
    AU_CSS_CHANGE_CHILDREN, /* children inserted or removed */
    AU_CSS_CHANGE_INLINE_STYLE,
    AU_CSS_CHANGE_ALL       /* sheets, device, fonts */
} AuCssChange;
void au_css_invalidate(AuCssResolver *r, AuElement *el, AuCssChange change, AuAtom attr);
/* child was inserted into parent (already linked), or removed from it
 * (prev and next were its siblings, child is detached). Call it during
 * the change, before anything else moves. Restyles the inserted subtree,
 * and only the elements selectors can reach from the change: parent for
 * :empty; the following siblings for :first-child, :nth-child(), the
 * of-type forms, :only-*, + and ~; the preceding siblings for
 * :last-child, :nth-last-child() and the like; the :has() anchors among
 * parent, its ancestors and their preceding siblings; the form elements
 * above whose state follows their descendants. Comments change nothing. */
void au_css_invalidate_children(AuCssResolver *r, AuNode *parent, AuNode *child, AuNode *prev, AuNode *next,
                                bool inserted);
/* Restyles el and its flat subtree: what a slot assignment or a shadow
 * root's children changed below el reaches. */
void au_css_invalidate_subtree(AuCssResolver *r, AuElement *el);
/* The data of text node text changed: :empty of its parent, :has()
 * arguments with :empty, a textarea's :placeholder-shown. */
void au_css_invalidate_text(AuCssResolver *r, AuNode *text);
/* For tests and the differential check: restyles the whole document and
 * returns false, with what differs in msg, when an element's computed
 * values were not what a whole restyle gives (call it after
 * au_css_restyle). */
bool au_css_check_restyle(AuCssResolver *r, AuDocument *doc, AuBuf *msg);
/* The AU_STATE_* bits in bits changed on el (already set or cleared; call
 * it for every element whose bits changed, ancestors included). Restyles
 * el, and only the other elements that selectors reading those states can
 * reach from el: its descendants or following siblings when a rule has the
 * state left of a combinator, and the ancestors or preceding siblings that
 * a :has() argument with the state can anchor on. el's subtree is restyled
 * when its style comes out different. AU_CSS_CHANGE_STATE is this call
 * with every bit. */
void au_css_invalidate_state(AuCssResolver *r, AuElement *el, uint32_t bits);
/* Moves a state that an element shares with its ancestors (AU_STATE_HOVER,
 * AU_STATE_ACTIVE) from one element to another; either may be NULL. Sets
 * and clears the bit and invalidates only the elements whose bit changed:
 * those below the nearest common ancestor. r may be NULL. */
void au_css_move_state(AuCssResolver *r, AuElement *from, AuElement *to, uint32_t bit);

/* ---- Selectors (querySelector, matches) ---- */
typedef struct AuCssSelectorList AuCssSelectorList;
/* Returns NULL if the selector list is invalid. */
AuCssSelectorList *au_css_selector_parse(const char *text, size_t len);
void au_css_selector_free(AuCssSelectorList *l);
/* Element.matches: :scope is el itself. */
bool au_css_selector_matches(const AuCssSelectorList *l, AuElement *el);
/* The same with :scope (and a bare & ) standing for scope: the element
 * querySelectorAll or closest was called on. A scope that is not an
 * element (a document or fragment, or NULL) makes :scope match the root
 * element, as :root does. au_css_query_selector uses its root. */
bool au_css_selector_matches_scoped(const AuCssSelectorList *l, AuElement *el, const AuNode *scope);
/* The first matching element in root's subtree (preorder), or NULL. */
AuElement *au_css_query_selector(const AuCssSelectorList *l, AuNode *root);

/* ---- Library ---- */
/* Interns the CSS names. Called by every entry point; safe to call again. */
void au_css_init(void);

#endif
