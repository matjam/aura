/* Aura SVG: a static SVG renderer for icons, logos and illustrations.
 *
 * One renderer serves two kinds of tree, both built from AuElement nodes
 * in the SVG namespace with computed styles (AuElement.style):
 *
 * - Inline SVG: the HTML parser builds the elements and the page's CSS
 *   resolver styles them, so page rules (fill: currentColor, svg { width:
 *   1em }) apply. Layout lays the outer svg out as a replaced element and
 *   calls au_svg_paint with its content box.
 * - SVG documents (files, data: URLs, SVG as an image): au_svg_doc_new
 *   parses the bytes with the XML parser in xml.c and styles the tree with
 *   the document's own <style> sheets through the same CSS engine.
 *   au_svg_doc_render draws it at any size.
 *
 * Painting draws vector geometry straight into the canvas at the size it
 * is shown (no fixed-size rasterization), so icons stay sharp at any zoom.
 * An offscreen group is used only for group opacity, masks and non-
 * rectangular clip paths.
 *
 * Supported: svg (nested, viewBox, preserveAspectRatio), g, a, switch,
 * path (the full path grammar, arcs as cubics), rect (rounded), circle,
 * ellipse, line, polyline, polygon, use (same-document references, with
 * cycle and depth limits), defs, symbol, linearGradient and
 * radialGradient (stops, units, gradientTransform, spreadMethod, href
 * inheritance, focal points; the focal radius fr is ignored), clipPath
 * (shapes, text and use children, clip-rule, units), mask (luminance and
 * alpha, units), image (raster images, nested SVG at a bounded depth),
 * text and tspan (one line: x, y, dx, dy lists, text-anchor,
 * dominant-baseline, letter-spacing, font properties), fill and stroke
 * with every stroke property, opacity, the transform attribute and the CSS
 * transform property, visibility, display, vector-effect:
 * non-scaling-stroke and paint-order. In inline SVG, a foreignObject
 * draws the first HTML <img> in it over its box, with its mask and clip.
 *
 * Not supported: filters (the element draws unfiltered), patterns (the
 * paint's fallback colour is used), markers, textPath, other HTML in a
 * foreignObject, multi-line text layout, SMIL animation, CSS basic shapes in clip-path,
 * external references (use or gradients in other files).
 *
 * Limits for hostile input: element nesting (SVG_MAX_DEPTH), use nesting
 * and cycles, elements painted per call, href chains, nested SVG images
 * (depth 2), and the XML parser's own limits (xml.c).
 *
 * Engine rules: no OS calls, no global mutable state, recursion guarded. */
#ifndef AURA_SVG_H
#define AURA_SVG_H

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#endif
#include "../css/css.h"
#include "../paint/font.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* ---- XML (xml.c) ---- */

typedef struct AuXmlLimits {
    uint32_t max_depth;    /* element nesting; 0: 256 */
    uint32_t max_nodes;    /* elements and text nodes; 0: 100000 */
    uint32_t max_attrs;    /* attributes per element; 0: 256 */
} AuXmlLimits;

/* Parses an SVG document (UTF-8; a BOM is skipped). Elements are created
 * in the SVG namespace with their local names (an svg: prefix is dropped;
 * other prefixes stay in the name, so such elements are never rendered);
 * xlink:href and xml:space keep their qualified names with their
 * attribute namespaces. Comments, processing instructions and the doctype
 * (internal subset included) are skipped; entity declarations are never
 * read, so only the five predefined entities and character references
 * expand (an unknown reference stays as written). Whitespace-only text is
 * kept only inside text, tspan and style. Returns NULL, with *error set
 * when error is not NULL, for input that is not well formed or exceeds a
 * limit. The caller releases the document with au_node_unref. */
AuDocument *au_xml_parse(const char *data, size_t len, const AuXmlLimits *lim, const char **error);

/* ---- Path data and attribute values (pathdata.c) ---- */

/* Appends the path of an SVG d attribute to out. Returns false when the
 * data has an error; the path then holds everything before it (SVG renders
 * up to the error). */
bool au_svg_path_parse(const char *d, size_t len, AuPath *out);
/* Parses a transform list (the attribute grammar: unitless numbers,
 * rotate(a cx cy), commas or spaces). False, with *m the identity, on an
 * error (the attribute is then ignored). */
bool au_svg_transform_parse(const char *s, AuMatrix *m);

/* ---- Rendering (render.c) ---- */

/* The atoms the renderer compares names with, interned once. Keep one
 * per layout or document. */
typedef struct AuSvgNames AuSvgNames;
AuSvgNames *au_svg_names_new(void);
void au_svg_names_free(AuSvgNames *n);

typedef struct AuSvgEnv {
    AuFontSet *fonts; /* for text; NULL draws no text */
    void *ctx;
    /* The pixels for an <image> href (as written), wanted at about dw x dh
     * device pixels; NULL while loading or when it cannot load. The
     * surface must stay valid until au_svg_paint returns. Optional. */
    const AuSurface *(*image)(void *ctx, const AuElement *el, const char *href, int dw, int dh);
    /* The element with an id in the document. Optional; without it the
     * renderer indexes the subtree of the svg it paints. */
    AuElement *(*find_id)(void *ctx, AuAtom id);
} AuSvgEnv;

/* An index of the elements with an id under a node (the node included),
 * for AuSvgEnv.find_id: the first element in tree order wins. Build it
 * again when the tree or ids change. */
typedef struct AuSvgIdIndex AuSvgIdIndex;
AuSvgIdIndex *au_svg_id_index_new(AuNode *root);
AuElement *au_svg_id_index_find(const AuSvgIdIndex *x, AuAtom id);
void au_svg_id_index_free(AuSvgIdIndex *x);

/* The intrinsic size of an outer svg element from its width and height
 * attributes (absolute lengths; font-relative ones against em) and the
 * ratio from width and height, else from viewBox. 0 for what is absent. */
typedef struct AuSvgIntrinsic {
    float width, height, ratio;
} AuSvgIntrinsic;
void au_svg_intrinsic(const AuSvgNames *n, const AuElement *svg, float em, AuSvgIntrinsic *out);

/* Paints svg (an svg element) with its viewport at view (canvas
 * coordinates). Clips to view unless the element's overflow is visible. */
void au_svg_paint(AuCanvas *c, const AuSvgNames *n, AuElement *svg, AuRect view, const AuSvgEnv *env);

/* ---- SVG documents (svgdoc.c) ---- */

#define AU_SVG_MAX_BYTES ((size_t)8 * 1024 * 1024)

/* Whether bytes look like an SVG document: optional BOM and whitespace,
 * then <?xml, <!--, <!DOCTYPE svg or <svg (comments and processing
 * instructions before the root are skipped while looking). */
bool au_svg_sniff(const void *data, size_t len);

typedef struct AuSvgDoc AuSvgDoc;
/* Parses and styles an SVG document. NULL when it is not one (the root
 * must be an svg element) or is malformed. depth counts nested SVG images
 * (0 for a top-level image); a nested SVG at depth 2 or more is refused.
 * The document never loads anything: <image> draws only data: URLs, and
 * scripts are ignored. */
AuSvgDoc *au_svg_doc_new(const void *data, size_t len, int depth);
void au_svg_doc_free(AuSvgDoc *d);
void au_svg_doc_intrinsic(const AuSvgDoc *d, AuSvgIntrinsic *out);
/* The size an <img> shows it at with no CSS size: the intrinsic size,
 * one side from the other through the ratio, else 300 x 150 (the ratio
 * kept when only that is known). */
void au_svg_doc_default_size(const AuSvgDoc *d, int *w, int *h);
/* Draws the document with its viewport at view. */
void au_svg_doc_render(AuSvgDoc *d, AuCanvas *c, AuRect view, AuFontSet *fonts);
/* A w x h premultiplied surface of the document (NULL for a bad size). */
AuSurface *au_svg_doc_rasterize(AuSvgDoc *d, int w, int h, AuFontSet *fonts);
/* Elements and text nodes in the document, and an estimate of the bytes
 * its tree and styles hold. */
void au_svg_doc_stats(const AuSvgDoc *d, uint32_t *nodes, size_t *bytes);

/* Decodes a data: URL's payload (base64 or percent-encoded) into a new
 * buffer of at most max bytes; *mime gets the media type (lowercase,
 * without parameters, truncated to cap bytes). False for anything that
 * is not a data: URL. */
bool au_svg_data_url(const char *url, size_t max, uint8_t **out, size_t *out_len, char *mime, size_t cap);

#endif
