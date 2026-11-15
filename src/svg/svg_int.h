/* SVG internals shared by the files of src/svg. */
#ifndef AURA_SVG_INT_H
#define AURA_SVG_INT_H

#include "svg.h"

#include <math.h>
#include <string.h>

/* Limits. */
#define SVG_MAX_DEPTH 48       /* element nesting while painting, use instances included */
#define SVG_MAX_USE_DEPTH 12   /* use inside use */
#define SVG_MAX_ELEMENTS 200000 /* elements painted per au_svg_paint */
#define SVG_MAX_HREF_CHAIN 16  /* gradient href inheritance */
#define SVG_MAX_IMAGE_DEPTH 2  /* SVG images inside SVG images */
#define SVG_MAX_TEXT_CHARS 4096 /* characters of one text element */

/* Names (element and attribute), as AuSvgNames fields: N(field, "name"). */
#define SVG_NAMES(N)                                                                                  \
    N(svg, "svg") N(g, "g") N(path, "path") N(rect, "rect") N(circle, "circle") N(ellipse, "ellipse")    \
    N(line, "line") N(polyline, "polyline") N(polygon, "polygon") N(use, "use") N(defs, "defs")          \
    N(symbol, "symbol") N(linear_gradient, "linearGradient") N(radial_gradient, "radialGradient")       \
    N(stop, "stop") N(clip_path, "clipPath") N(mask, "mask") N(image, "image") N(text, "text")          \
    N(tspan, "tspan") N(a, "a") N(switch_, "switch") N(foreign_object, "foreignObject")                \
    N(style, "style") N(pattern, "pattern")                                                             \
    N(x, "x") N(y, "y") N(width, "width") N(height, "height") N(rx, "rx") N(ry, "ry") N(cx, "cx")         \
    N(cy, "cy") N(r, "r") N(fx, "fx") N(fy, "fy") N(x1, "x1") N(y1, "y1") N(x2, "x2") N(y2, "y2")       \
    N(d, "d") N(points, "points") N(view_box, "viewBox") N(par, "preserveAspectRatio")                  \
    N(transform, "transform") N(href, "href") N(xlink_href, "xlink:href")                               \
    N(gradient_units, "gradientUnits") N(gradient_transform, "gradientTransform")                       \
    N(spread_method, "spreadMethod") N(offset, "offset") N(clip_path_units, "clipPathUnits")            \
    N(mask_units, "maskUnits") N(mask_content_units, "maskContentUnits") N(dx, "dx") N(dy, "dy")        \
    N(required_extensions, "requiredExtensions") N(required_features, "requiredFeatures")               \
    N(system_language, "systemLanguage") N(xml_space, "xml:space") N(media, "media")

struct AuSvgNames {
#define N(f, s) AuAtom f;
    SVG_NAMES(N)
#undef N
};

/* Lengths of attributes: a number with an optional unit. em and ex need
 * the font size; % the reference length. */
typedef struct SvgLen {
    float v;
    uint8_t unit; /* SVG_U_* */
} SvgLen;
enum { SVG_U_NUM, SVG_U_PX, SVG_U_PT, SVG_U_PC, SVG_U_MM, SVG_U_CM, SVG_U_IN, SVG_U_EM, SVG_U_EX, SVG_U_PCT };

/* pathdata.c */
/* Parses a number at *p (SVG number grammar); advances *p. */
bool svg_parse_number(const char **p, float *out);
/* Skips whitespace and at most one comma. */
void svg_skip_sep(const char **p);
bool svg_parse_length(const char *s, SvgLen *out);
float svg_len_px(SvgLen l, float em, float ref);
/* A list of numbers (points, viewBox, x lists): returns how many were
 * read into out (at most cap); *ok false on garbage. */
size_t svg_parse_numbers(const char *s, float *out, size_t cap, bool *ok);
/* A list of lengths. */
size_t svg_parse_lengths(const char *s, SvgLen *out, size_t cap);

/* preserveAspectRatio */
enum { SVG_ALIGN_NONE, SVG_ALIGN_MIN, SVG_ALIGN_MID, SVG_ALIGN_MAX };
typedef struct SvgPar {
    uint8_t ax, ay; /* SVG_ALIGN_*; NONE: scale non-uniformly */
    bool slice;
} SvgPar;
void svg_parse_par(const char *s, SvgPar *out);
/* The matrix that maps viewBox vb into viewport vp. */
AuMatrix svg_viewbox_matrix(const float vb[4], AuRect vp, SvgPar par);

static inline bool svg_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* ---- Rendering (render.c, svgtext.c) ---- */

/* The inherited painting state of an element, from its computed style.
 * Inside a use instance an inherited value the element did not set comes
 * from the instance's parent instead of the element's DOM parent (see
 * svg_state in render.c). */
typedef struct SvgState {
    const AuCssBlob *fill, *stroke, *dash, *order, *family, *fvs;
    AuLen sw, dashoff;
    float fill_op, stroke_op, miter, font_size, letter_spacing, stretch;
    uint32_t color; /* CSS RGBA */
    uint16_t weight;
    uint8_t fill_rule, clip_rule, cap, join, anchor, visibility, font_style, opsz;
} SvgState;

typedef struct SvgFrame {
    AuMatrix ctm;  /* user space to canvas */
    float vw, vh;  /* the nearest viewport, user units (percentages) */
    AuRect vp_dev; /* the nearest viewport on the canvas (group bounds) */
    SvgState st;
    bool inst;     /* inside a use instance */
} SvgFrame;

typedef struct SvgIdEntry {
    AuAtom id;
    uint32_t order;
    AuElement *el;
} SvgIdEntry;

typedef struct SvgCtx {
    AuCanvas *c;
    const AuSvgEnv *env;
    const AuSvgNames *n;
    AuElement *root;      /* the outermost svg painted */
    AuSvgIdIndex *ids;    /* own id index when env->find_id is NULL (built on first use) */
    int depth, use_depth;
    uint32_t painted;     /* elements visited (SVG_MAX_ELEMENTS) */
    AuElement *use_stack[SVG_MAX_USE_DEPTH + 1];
    SvgFrame *text_spans; /* svgtext.c: the frames of the text being laid out */
} SvgCtx;

/* Appends src's segments to dst. */
void svg_path_append(AuPath *dst, const AuPath *src);

/* An element's attribute by atom (NULL when absent or the atom is 0). */
const char *svg_attr(const AuElement *el, AuAtom a);
float svg_len_attr(const AuElement *el, AuAtom a, const SvgFrame *f, int axis, float dflt);
void svg_state(const AuElement *el, const SvgFrame *parent, bool inst, SvgState *out);
/* Fills and strokes path (user space) with the frame's state; bbox is
 * the object bounding box (user space) for gradients. */
void svg_paint_geometry(SvgCtx *x, const AuElement *el, const SvgFrame *f, const AuPath *path, const float bbox[4],
                        float opacity, bool closed_hint);
/* text (svgtext.c): paints a text element, or with bbox only measures. */
void svg_text_paint(SvgCtx *x, AuElement *el, const SvgFrame *f, float opacity);
bool svg_text_bbox(SvgCtx *x, AuElement *el, const SvgFrame *f, float bbox[4]);
/* The text's glyph outlines in user space (clip paths). */
void svg_text_outline(SvgCtx *x, AuElement *el, const SvgFrame *f, AuPath *out);
bool svg_is_tag(const AuElement *el, AuAtom tag);

#endif
