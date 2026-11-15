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

/* ---- Paint sources ---- */

#define AU_GRADIENT_MAX_STOPS 16

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
/* An affine transform: x' = a x + c y + e, y' = b x + d y + f. */
typedef struct AuMatrix {
    float a, b, c, d, e, f;
} AuMatrix;
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
