/* Aura paint: surfaces, paths, the coverage rasterizer and the canvas that
 * CSS painting draws with. Portable C99; nothing here calls the OS.
 *
 * Pixel format
 * ------------
 * A pixel is one uint32_t holding 0xAARRGGBB with premultiplied alpha, in
 * the CPU's native byte order. Code reads and writes channels with shifts,
 * never by byte address, so the same code runs on both byte orders:
 *
 * - On big-endian PowerPC the bytes in memory are A, R, G, B. That is the
 *   layout of a 32-bit QuickDraw GWorld (xRGB; QuickDraw ignores the first
 *   byte), so the Mac OS 9 shell points a surface at the GWorld's pixels and
 *   CopyBits it to the window with no per-pixel conversion.
 * - On a little-endian host the bytes are B, G, R, A. The PNG writer
 *   converts when it writes a file.
 *
 * Colours given to the API (AuColor) are CSS colours: 0xAARRGGBB, not
 * premultiplied.
 *
 * Coordinates are floats in CSS pixels with y down. A canvas has a
 * translation (au_canvas_translate) and a stack of clip rectangles.
 * Geometry is antialiased: a rectangle edge at x = 10.5 covers half of
 * column 10.
 *
 * Numbers: the rasterizer accumulates coverage in float (the G3 and G4 have
 * a fast FPU); the compositing loops are 8-bit integer, two channels per
 * 32-bit operation. Places where AltiVec would help later are marked
 * "AltiVec:" in the sources. */
#ifndef AURA_PAINT_H
#define AURA_PAINT_H

#include "../core/core.h"

typedef uint32_t AuPixel; /* 0xAARRGGBB, premultiplied, native byte order */
typedef uint32_t AuColor; /* 0xAARRGGBB, not premultiplied */

#define AU_RGBA(r, g, b, a) \
    ((AuColor)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define AU_RGB(r, g, b) AU_RGBA(r, g, b, 255)

typedef struct AuRect {
    float x, y, w, h;
} AuRect;

/* Integer rectangle, half-open: x0 <= x < x1, y0 <= y < y1. */
typedef struct AuIRect {
    int x0, y0, x1, y1;
} AuIRect;

/* ---- Surfaces ---- */

typedef struct AuSurface {
    AuPixel *pixels;
    int width, height;
    int stride; /* pixels from one row to the next */
    bool owned; /* pixels belong to the surface */
    bool opaque; /* every pixel has alpha 255 (decoded images): drawing may skip blending */
} AuSurface;

/* A new surface, cleared to transparent. */
AuSurface *au_surface_new(int width, int height);
/* A surface over memory the caller owns (a GWorld's pixmap, for example).
 * stride_bytes must be a multiple of 4. */
AuSurface *au_surface_wrap(void *pixels, int width, int height, int stride_bytes);
void au_surface_free(AuSurface *s);
void au_surface_clear(AuSurface *s, AuColor c);
/* Reads a pixel back as a non-premultiplied colour (tests, PNG output). */
AuColor au_surface_get(const AuSurface *s, int x, int y);

/* ---- Paths ----
 * Points are floats. Curves are kept as curves until the rasterizer
 * flattens them. A path may hold any number of subpaths. */

typedef enum { AU_FILL_NONZERO, AU_FILL_EVENODD } AuFillRule;

typedef struct AuPath {
    uint8_t *ops; /* AU_PATH_* below */
    float *pts;   /* x, y pairs: 1 per move/line, 2 per quad, 3 per cubic */
    size_t nops, npts, ops_cap, pts_cap;
    float sx, sy; /* start of the current subpath */
    float cx, cy; /* current point */
} AuPath;

enum { AU_PATH_MOVE, AU_PATH_LINE, AU_PATH_QUAD, AU_PATH_CUBIC, AU_PATH_CLOSE };

void au_path_init(AuPath *p);
void au_path_free(AuPath *p);
void au_path_clear(AuPath *p);
void au_path_move_to(AuPath *p, float x, float y);
void au_path_line_to(AuPath *p, float x, float y);
void au_path_quad_to(AuPath *p, float cx, float cy, float x, float y);
void au_path_cubic_to(AuPath *p, float c1x, float c1y, float c2x, float c2y, float x, float y);
void au_path_close(AuPath *p);

/* Per-corner elliptical radii: top-left, top-right, bottom-right,
 * bottom-left (the CSS order). */
enum { AU_CORNER_TL, AU_CORNER_TR, AU_CORNER_BR, AU_CORNER_BL };
typedef struct AuRadii {
    float rx[4], ry[4];
} AuRadii;

/* Scales radii down so adjacent ones do not overlap (CSS Backgrounds 3,
 * "corner overlap"). A NULL or all-zero radii set is a plain rectangle. */
void au_radii_fit(AuRadii *r, AuRect box);
bool au_radii_zero(const AuRadii *r);

void au_path_rect(AuPath *p, AuRect r);
/* Clockwise; reverse adds it counter-clockwise (a hole under nonzero). */
void au_path_rounded_rect(AuPath *p, AuRect r, const AuRadii *radii, bool reverse);
void au_path_ellipse(AuPath *p, float cx, float cy, float rx, float ry);
/* Appends to out the outline of in stroked with the given width: butt
 * caps, round joins. Fill the result with AU_FILL_NONZERO. */
void au_path_stroke(const AuPath *in, float width, AuPath *out);

/* An affine transform: x' = a x + c y + e, y' = b x + d y + f. */
typedef struct AuMatrix {
    float a, b, c, d, e, f;
} AuMatrix;

/* m = m * n: n applies first, then m. */
void au_matrix_multiply(AuMatrix *m, const AuMatrix *n);
/* False (and *out unchanged) when m is not invertible. */
bool au_matrix_invert(const AuMatrix *m, AuMatrix *out);
/* Maps every point of p through m, in place. */
void au_path_transform(AuPath *p, const AuMatrix *m);
/* The bounding box of p's points (control points included): x0, y0, x1,
 * y1. False for an empty path. */
bool au_path_bounds(const AuPath *p, float b[4]);

/* An SVG elliptical arc from the current point to (x, y) (SVG 1.1
 * appendix F.6): radii rx, ry, x-axis rotation in degrees, the large-arc
 * and sweep flags. Radii too small to reach are scaled up; a zero radius
 * gives a line. Appended as cubics, one per quarter turn or less. */
void au_path_arc_svg(AuPath *p, float rx, float ry, float rot_deg, bool large, bool sweep, float x, float y);

/* Stroking with caps, joins and dashes (stroke.c). The outline is a
 * union of pieces wound the same way: fill it with AU_FILL_NONZERO. */
typedef enum { AU_CAP_BUTT, AU_CAP_ROUND, AU_CAP_SQUARE } AuLineCap;
typedef enum { AU_JOIN_MITER, AU_JOIN_ROUND, AU_JOIN_BEVEL } AuLineJoin;
#define AU_STROKE_MAX_DASHES 64
typedef struct AuStrokeStyle {
    float width;
    uint8_t cap, join;   /* AuLineCap, AuLineJoin */
    float miter_limit;   /* ratio of miter length to width; below 1 counts as 1 */
    const float *dashes; /* NULL or ndashes lengths, on and off alternately */
    size_t ndashes;      /* an odd count repeats the list; at most AU_STROKE_MAX_DASHES */
    float dash_offset;
    float tolerance;     /* flattening tolerance in path units; 0: 0.1 */
} AuStrokeStyle;
void au_path_stroke_style(const AuPath *in, const AuStrokeStyle *st, AuPath *out);

/* ---- Paint sources ---- */

#define AU_GRADIENT_MAX_STOPS 32

/* Spread methods beyond the 0..1 range. */
enum { AU_SPREAD_PAD, AU_SPREAD_REFLECT, AU_SPREAD_REPEAT };

typedef struct AuGradientStop {
    float pos; /* 0..1 along the gradient line */
    AuColor color;
} AuGradientStop;

/* A linear gradient from (x0, y0) to (x1, y1), or (radial) an elliptical
 * one centred on (x0, y0) with radii (rx, ry), in canvas coordinates. A
 * repeating gradient repeats its 0..1 stop range along the line (or
 * outwards). Zero-initialize, then set the fields. */
typedef struct AuGradient {
    float x0, y0, x1, y1;
    float rx, ry;
    bool radial, repeat;
    int nstops;
    AuGradientStop stops[AU_GRADIENT_MAX_STOPS];
    /* SVG additions; zero leaves the behaviour above.
     * spread: AU_SPREAD_REFLECT mirrors each period (repeat set or
     * AU_SPREAD_REPEAT repeats it).
     * has_matrix: the geometry above is in a gradient space that matrix
     * maps to canvas coordinates (SVG gradientTransform and
     * objectBoundingBox units).
     * focal: a radial gradient's 0 position is the point (fx, fy) instead
     * of the centre (the circle of radius rx at (x0, y0) is still the 1
     * position; ry is ignored). A focal point on or outside the circle is
     * moved just inside it, as SVG 1.1 says. */
    uint8_t spread;
    bool has_matrix;
    AuMatrix matrix;
    bool focal;
    float fx, fy;
} AuGradient;

/* Sets the gradient line for a CSS linear-gradient(angle) over box:
 * 0deg points up, 90deg right; the line is long enough that the corners
 * get the first and last colours. */
void au_gradient_css_line(AuGradient *g, AuRect box, float angle_deg);
void au_gradient_add_stop(AuGradient *g, float pos, AuColor c);

/* ---- Canvas ---- */

typedef struct AuCanvas AuCanvas;

AuCanvas *au_canvas_new(AuSurface *target);
void au_canvas_free(AuCanvas *c);
AuSurface *au_canvas_target(AuCanvas *c);

/* Moves the origin by (dx, dy). Clip rectangles already pushed stay where
 * they are on the surface. */
void au_canvas_translate(AuCanvas *c, float dx, float dy);
void au_canvas_get_translation(const AuCanvas *c, float *dx, float *dy);

/* Clip stack: each push intersects the current clip with r (rounded to
 * whole pixels). Pops must match pushes. */
void au_canvas_clip_push(AuCanvas *c, AuRect r);
void au_canvas_clip_pop(AuCanvas *c);
/* The current clip on the base surface, in pixels. */
AuIRect au_canvas_clip(const AuCanvas *c);

/* Opacity groups: drawing between begin and end goes to a temporary
 * surface covering bounds (clipped), which end composites with opacity
 * (0..1). Groups nest. */
void au_canvas_group_begin(AuCanvas *c, AuRect bounds);
void au_canvas_group_end(AuCanvas *c, float opacity);
/* Masks: begin a group for the content, then a second group over the same
 * bounds for the mask, draw the mask, and call au_canvas_mask_end. It ends
 * the mask group and multiplies the content group's pixels by the mask's
 * alpha (luminance false) or by its luminance times alpha (luminance true,
 * the SVG mask element: linear coefficients 0.2125, 0.7154, 0.0721 on the
 * stored values). The caller then ends the content group. With fewer than
 * two groups open it only ends the innermost one. */
void au_canvas_mask_end(AuCanvas *c, bool luminance);
/* Transformed layers: drawing between begin and end goes to a temporary
 * surface covering bounds (whole pixels, not limited by the current
 * clip), which end draws through m (canvas coordinates, before the
 * canvas translation, like every other coordinate) with bilinear
 * filtering and the given opacity, clipped by the clip that was current
 * at begin. begin returns false, and pushes nothing, when bounds is
 * empty, not finite, or larger than 4 megapixels; the caller then draws
 * untransformed. Layers and groups nest. */
bool au_canvas_layer_begin(AuCanvas *c, AuRect bounds);
void au_canvas_layer_end(AuCanvas *c, const AuMatrix *m, float opacity);
/* Multiplies the innermost group's pixels by the coverage of p (canvas
 * coordinates, nonzero rule): pixels outside p become transparent. This
 * gives a clip of any shape (rounded overflow clipping). No-op outside a
 * group. */
void au_canvas_group_mask(AuCanvas *c, const AuPath *p);

void au_fill_rect(AuCanvas *c, AuRect r, AuColor color);
void au_fill_rect_gradient(AuCanvas *c, AuRect r, const AuGradient *g);
void au_fill_path(AuCanvas *c, const AuPath *p, AuFillRule rule, AuColor color);
void au_fill_path_gradient(AuCanvas *c, const AuPath *p, AuFillRule rule, const AuGradient *g);
void au_fill_rounded_rect(AuCanvas *c, AuRect r, const AuRadii *radii, AuColor color);

/* A solid border: widths and colours in CSS order (top, right, bottom,
 * left), drawn inside box, following radii (NULL for square corners).
 * Other border styles (dashed, dotted, double, groove...) come later. */
enum { AU_SIDE_TOP, AU_SIDE_RIGHT, AU_SIDE_BOTTOM, AU_SIDE_LEFT };
typedef struct AuBorder {
    float width[4];
    AuColor color[4];
} AuBorder;
void au_draw_border(AuCanvas *c, AuRect box, const AuBorder *b, const AuRadii *radii);

/* CSS box-shadow for a box with the given radii (NULL for square). blur is
 * the CSS blur radius; the shadow is approximated with three box blurs.
 * An outer shadow is not drawn under the box; an inset one only inside. */
/* The largest blur radius drawn; a larger one is drawn as this. */
#define AU_SHADOW_MAX_BLUR 300.0f
void au_draw_box_shadow(AuCanvas *c, AuRect box, const AuRadii *radii, float dx, float dy,
                        float blur, float spread, AuColor color, bool inset);

typedef enum { AU_FILTER_NEAREST, AU_FILTER_BILINEAR } AuFilter;
/* Draws the src rectangle of img (premultiplied) scaled into dst. */
void au_draw_image(AuCanvas *c, const AuSurface *img, AuIRect src, AuRect dst, AuFilter filter,
                   float opacity);

/* Blends color through an 8-bit coverage mask whose top-left pixel lands
 * at (x, y) in canvas coordinates (the translation applies; x and y are
 * rounded). Text drawing uses this. */
void au_draw_mask(AuCanvas *c, const uint8_t *mask, int w, int h, int stride, float x, float y,
                  AuColor color);

/* ---- Rasterizer ----
 * Lower-level access, for code that wants coverage rather than pixels
 * (masks, tests). The rasterizer flattens p translated by (dx, dy), and
 * calls fn once per run of non-zero coverage inside clip, row by row from
 * the top. cov[i] (1..255) is the coverage of pixel x + i in row y. */
typedef void (*AuSpanFn)(void *ctx, int y, int x, int n, const uint8_t *cov);
void au_raster_path(const AuPath *p, float dx, float dy, AuFillRule rule, AuIRect clip, AuSpanFn fn,
                    void *ctx);

/* An 8-bit coverage mask of p over the pixel rectangle area (path
 * coordinates are pixels on the same grid). mask has
 * (area.x1 - area.x0) * (area.y1 - area.y0) bytes, zeroed by the caller. */
void au_raster_mask(const AuPath *p, AuFillRule rule, AuIRect area, uint8_t *mask);

#endif
