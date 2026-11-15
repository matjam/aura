/* The 2D canvas context (CanvasRenderingContext2D of the HTML standard) as
 * a state machine in C. Drawing calls act on a premultiplied AuSurface
 * that the canvas owns; src/js binds script to these functions and layout
 * paints the surface into the canvas element's box.
 *
 * Numbers. Every coordinate argument is a double as script gave it. A call
 * with an argument that is not finite does nothing, as the standard says.
 * Device coordinates are clamped to +-900000 so that no value reaches the
 * rasterizer that it would drop.
 *
 * Memory. A canvas is at most AU_C2D_MAX_DIM pixels on a side and
 * AU_C2D_MAX_BYTES of pixels; a larger one is inert (drawing does nothing,
 * reading gives transparent pixels, au_c2d_surface is NULL). The bitmap is
 * allocated at the first call that draws, with the fallible allocator, and
 * only while the set's canvases, bitmaps and clip masks stay within the
 * set's budget; when it cannot be allocated the canvas behaves as an inert
 * one until a later draw succeeds.
 *
 * Bounds. A path holds at most AU_C2D_MAX_PATH_OPS segments (further
 * segments are dropped); a fill or stroke whose flattened outline would
 * exceed AU_C2D_MAX_EDGES edges, or whose stroke would walk more than
 * AU_C2D_MAX_STROKE_POINTS points, draws nothing; a gradient keeps
 * AU_C2D_MAX_STOPS stops; a dash list keeps AU_STROKE_MAX_DASHES entries
 * (after an odd list is doubled) and a longer one is ignored; the state
 * stack holds AU_C2D_MAX_STATES entries.
 *
 * Portable C99, no OS calls. */
#ifndef AURA_CANVAS2D_H
#define AURA_CANVAS2D_H

#include "font.h"

#define AU_C2D_MAX_DIM 4096
#define AU_C2D_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define AU_C2D_SET_BUDGET ((size_t)32 * 1024 * 1024)
#define AU_C2D_MAX_PATH_OPS 250000
#define AU_C2D_MAX_EDGES 500000
#define AU_C2D_MAX_STROKE_POINTS 60000
#define AU_C2D_MAX_STOPS 256
#define AU_C2D_MAX_STATES 4096
#define AU_C2D_MAX_TEXT 16384 /* bytes of UTF-8 per text call */

typedef struct AuCanvasSet AuCanvasSet;
typedef struct AuCanvas2D AuCanvas2D;
typedef struct AuCanvasGradient AuCanvasGradient;
typedef struct AuCanvasPattern AuCanvasPattern;
typedef struct AuCanvasBitmap AuCanvasBitmap;

/* ---- Sets ----
 * The canvases of one document. The set counts their memory against a
 * budget, lists the canvases that were drawn to since the last paint, and
 * holds the fonts text is drawn with. Freeing the set detaches its
 * canvases and bitmaps: they stay valid objects that draw nothing. */
AuCanvasSet *au_canvas_set_new(size_t budget); /* 0: AU_C2D_SET_BUDGET */
void au_canvas_set_free(AuCanvasSet *s);
/* The fonts for text (NULL: none; text then measures 0 and draws
 * nothing). Call again with NULL before the font set goes, and whenever
 * its faces change: the set keeps font lists made from it. */
void au_canvas_set_fonts(AuCanvasSet *s, AuFontSet *fonts);
/* Bytes of pixels and clip masks the set holds. */
size_t au_canvas_set_bytes(const AuCanvasSet *s);
/* The canvas of an owner (an element), or NULL. */
AuCanvas2D *au_canvas_set_find(const AuCanvasSet *s, const void *owner);
/* Calls fn for each canvas drawn to since the last call, with the area
 * that changed in canvas pixels, and clears the marks. Returns the count. */
int au_canvas_set_take_dirty(AuCanvasSet *s, void (*fn)(void *ctx, AuCanvas2D *c, void *owner, AuIRect area),
                             void *ctx);
bool au_canvas_set_dirty(const AuCanvasSet *s);

/* ---- Bitmaps ----
 * Reference-counted pixels: ImageBitmap objects, decoded images, pattern
 * sources. nat_w and nat_h are the size script sees; the surface may be
 * smaller (an image decoded reduced), and drawing scales. */
struct AuCanvasBitmap {
    AuSurface *surf;
    int nat_w, nat_h;
    bool clean; /* origin-clean: drawing it does not taint a canvas */
    uint32_t refs;
    AuCanvasSet *set;
    AuCanvasBitmap *prev, *next;
    size_t bytes;
};
/* A transparent bitmap, or NULL when the size is not allowed or the set's
 * budget is used up. */
AuCanvasBitmap *au_cbitmap_new(AuCanvasSet *s, int w, int h);
/* Takes over a surface (a decoded image). NULL, with the surface freed,
 * when the budget is used up. */
AuCanvasBitmap *au_cbitmap_adopt(AuCanvasSet *s, AuSurface *surf, int nat_w, int nat_h);
AuCanvasBitmap *au_cbitmap_ref(AuCanvasBitmap *b);
void au_cbitmap_unref(AuCanvasBitmap *b);

/* ---- Canvases ---- */
/* owner identifies the canvas for au_canvas_set_find (NULL: none).
 * alpha false makes an opaque canvas (transparent black shows as black).
 * Never NULL; a size beyond the limits gives an inert canvas. */
AuCanvas2D *au_c2d_new(AuCanvasSet *s, void *owner, int w, int h, bool alpha);
void au_c2d_free(AuCanvas2D *c);
/* Sets the size, clears the bitmap and resets the state (the width and
 * height setters). */
void au_c2d_resize(AuCanvas2D *c, int w, int h);
int au_c2d_width(const AuCanvas2D *c);
int au_c2d_height(const AuCanvas2D *c);
bool au_c2d_alpha(const AuCanvas2D *c);
/* The bitmap, or NULL while nothing was drawn (or the canvas is inert). */
const AuSurface *au_c2d_surface(const AuCanvas2D *c);
/* True when the size is over the limits. */
bool au_c2d_inert(const AuCanvas2D *c);
/* The origin-clean flag. */
bool au_c2d_clean(const AuCanvas2D *c);
void au_c2d_taint(AuCanvas2D *c);

/* State. */
void au_c2d_save(AuCanvas2D *c);
void au_c2d_restore(AuCanvas2D *c);
void au_c2d_reset(AuCanvas2D *c);

/* Transforms. */
void au_c2d_scale(AuCanvas2D *c, double x, double y);
void au_c2d_rotate(AuCanvas2D *c, double angle);
void au_c2d_translate(AuCanvas2D *c, double x, double y);
void au_c2d_transform(AuCanvas2D *c, double a, double b, double cc, double d, double e, double f);
void au_c2d_set_transform(AuCanvas2D *c, double a, double b, double cc, double d, double e, double f);
void au_c2d_get_transform(const AuCanvas2D *c, double m[6]);

/* Compositing. The setters with a name return false, changing nothing,
 * for a name the standard does not define. */
void au_c2d_set_alpha(AuCanvas2D *c, double a);
double au_c2d_get_alpha(const AuCanvas2D *c);
bool au_c2d_set_composite(AuCanvas2D *c, const char *name);
const char *au_c2d_get_composite(const AuCanvas2D *c);
void au_c2d_set_smoothing(AuCanvas2D *c, bool on);
bool au_c2d_get_smoothing(const AuCanvas2D *c);
bool au_c2d_set_smoothing_quality(AuCanvas2D *c, const char *name);
const char *au_c2d_get_smoothing_quality(const AuCanvas2D *c);

/* Styles. which: 0 fillStyle, 1 strokeStyle. A colour is 0xAARRGGBB, not
 * premultiplied. The canvas references the gradient or pattern. */
enum { AU_C2D_FILL = 0, AU_C2D_STROKE = 1 };
enum { AU_C2D_STYLE_COLOR, AU_C2D_STYLE_GRADIENT, AU_C2D_STYLE_PATTERN };
void au_c2d_set_color(AuCanvas2D *c, int which, AuColor color);
void au_c2d_set_gradient(AuCanvas2D *c, int which, AuCanvasGradient *g);
void au_c2d_set_pattern(AuCanvas2D *c, int which, AuCanvasPattern *p);
int au_c2d_style_kind(const AuCanvas2D *c, int which);
AuColor au_c2d_style_color(const AuCanvas2D *c, int which);
AuCanvasGradient *au_c2d_style_gradient(const AuCanvas2D *c, int which);
AuCanvasPattern *au_c2d_style_pattern(const AuCanvas2D *c, int which);

/* Gradients. Stops are 0..1 (the caller rejects others). user is for the
 * binding (the script object while it lives). */
AuCanvasGradient *au_cgradient_linear(double x0, double y0, double x1, double y1);
AuCanvasGradient *au_cgradient_radial(double x0, double y0, double r0, double x1, double y1, double r1);
AuCanvasGradient *au_cgradient_conic(double angle, double x, double y);
void au_cgradient_add_stop(AuCanvasGradient *g, double offset, AuColor color);
AuCanvasGradient *au_cgradient_ref(AuCanvasGradient *g);
void au_cgradient_unref(AuCanvasGradient *g);
void **au_cgradient_user(AuCanvasGradient *g);

/* Patterns. repeat: bit 0 repeats in x, bit 1 in y. */
AuCanvasPattern *au_cpattern_new(AuCanvasBitmap *b, int repeat);
void au_cpattern_set_transform(AuCanvasPattern *p, double a, double b, double c, double d, double e, double f);
AuCanvasPattern *au_cpattern_ref(AuCanvasPattern *p);
void au_cpattern_unref(AuCanvasPattern *p);
void **au_cpattern_user(AuCanvasPattern *p);
bool au_cpattern_clean(const AuCanvasPattern *p);

/* Lines. */
void au_c2d_set_line_width(AuCanvas2D *c, double w);
double au_c2d_get_line_width(const AuCanvas2D *c);
bool au_c2d_set_line_cap(AuCanvas2D *c, const char *name);
const char *au_c2d_get_line_cap(const AuCanvas2D *c);
bool au_c2d_set_line_join(AuCanvas2D *c, const char *name);
const char *au_c2d_get_line_join(const AuCanvas2D *c);
void au_c2d_set_miter_limit(AuCanvas2D *c, double m);
double au_c2d_get_miter_limit(const AuCanvas2D *c);
/* n lengths; a negative or non-finite one, or more than the cap, leaves
 * the list unchanged. An odd list is stored doubled. */
void au_c2d_set_line_dash(AuCanvas2D *c, const double *d, size_t n);
size_t au_c2d_get_line_dash(const AuCanvas2D *c, const float **d);
void au_c2d_set_dash_offset(AuCanvas2D *c, double o);
double au_c2d_get_dash_offset(const AuCanvas2D *c);

/* Shadows. */
void au_c2d_set_shadow_color(AuCanvas2D *c, AuColor color);
AuColor au_c2d_get_shadow_color(const AuCanvas2D *c);
void au_c2d_set_shadow_blur(AuCanvas2D *c, double b);
double au_c2d_get_shadow_blur(const AuCanvas2D *c);
void au_c2d_set_shadow_offset(AuCanvas2D *c, int axis, double v);
double au_c2d_get_shadow_offset(const AuCanvas2D *c, int axis);

/* ---- Paths ----
 * A path object (Path2D, and each context's current path). m maps the
 * coordinates given to the path's space: the context passes its
 * transform, a Path2D NULL. Each returns false for an argument the
 * standard answers with an exception (a negative radius). */
typedef struct AuCanvasPath {
    AuPath p;
} AuCanvasPath;
void au_cpath_init(AuCanvasPath *p);
void au_cpath_free(AuCanvasPath *p);
void au_cpath_clear(AuCanvasPath *p);
void au_cpath_move_to(AuCanvasPath *p, const AuMatrix *m, double x, double y);
void au_cpath_line_to(AuCanvasPath *p, const AuMatrix *m, double x, double y);
void au_cpath_quad_to(AuCanvasPath *p, const AuMatrix *m, double cx, double cy, double x, double y);
void au_cpath_cubic_to(AuCanvasPath *p, const AuMatrix *m, double c1x, double c1y, double c2x, double c2y,
                       double x, double y);
bool au_cpath_arc(AuCanvasPath *p, const AuMatrix *m, double x, double y, double r, double a0, double a1, bool ccw);
bool au_cpath_arc_to(AuCanvasPath *p, const AuMatrix *m, double x1, double y1, double x2, double y2, double r);
bool au_cpath_ellipse(AuCanvasPath *p, const AuMatrix *m, double x, double y, double rx, double ry, double rot,
                      double a0, double a1, bool ccw);
void au_cpath_rect(AuCanvasPath *p, const AuMatrix *m, double x, double y, double w, double h);
/* radii: x and y for the top-left, top-right, bottom-right and
 * bottom-left corners. False for a negative radius. */
bool au_cpath_round_rect(AuCanvasPath *p, const AuMatrix *m, double x, double y, double w, double h,
                         const double radii[8]);
void au_cpath_close(AuCanvasPath *p);
/* Appends src mapped through m (NULL: as it is). */
void au_cpath_add(AuCanvasPath *p, const AuCanvasPath *src, const AuMatrix *m);
/* Appends raw segments (SVG path data parsed by src/svg). */
void au_cpath_add_raw(AuCanvasPath *p, const AuPath *src);

/* The context's current path and transform, for the calls above. */
AuCanvasPath *au_c2d_path(AuCanvas2D *c);
const AuMatrix *au_c2d_matrix(const AuCanvas2D *c);
void au_c2d_begin_path(AuCanvas2D *c);

/* Drawing paths. path NULL: the current path; else a Path2D, mapped
 * through the current transform. */
void au_c2d_fill(AuCanvas2D *c, const AuCanvasPath *path, AuFillRule rule);
void au_c2d_stroke(AuCanvas2D *c, const AuCanvasPath *path);
void au_c2d_clip(AuCanvas2D *c, const AuCanvasPath *path, AuFillRule rule);
bool au_c2d_point_in_path(AuCanvas2D *c, const AuCanvasPath *path, double x, double y, AuFillRule rule);
bool au_c2d_point_in_stroke(AuCanvas2D *c, const AuCanvasPath *path, double x, double y);

/* Rectangles. */
void au_c2d_clear_rect(AuCanvas2D *c, double x, double y, double w, double h);
void au_c2d_fill_rect(AuCanvas2D *c, double x, double y, double w, double h);
void au_c2d_stroke_rect(AuCanvas2D *c, double x, double y, double w, double h);

/* ---- Text ----
 * The font: a CSS font-family value, weight, style and size in pixels,
 * and the text the font attribute reads back. */
void au_c2d_set_font(AuCanvas2D *c, const char *families, int weight, bool italic, double size, const char *css);
const char *au_c2d_get_font(const AuCanvas2D *c);
bool au_c2d_set_text_align(AuCanvas2D *c, const char *name);
const char *au_c2d_get_text_align(const AuCanvas2D *c);
bool au_c2d_set_text_baseline(AuCanvas2D *c, const char *name);
const char *au_c2d_get_text_baseline(const AuCanvas2D *c);
bool au_c2d_set_direction(AuCanvas2D *c, const char *name);
const char *au_c2d_get_direction(const AuCanvas2D *c);
/* which: 0 letterSpacing, 1 wordSpacing, in pixels. */
void au_c2d_set_spacing(AuCanvas2D *c, int which, double px);
double au_c2d_get_spacing(const AuCanvas2D *c, int which);
bool au_c2d_set_kerning(AuCanvas2D *c, const char *name);
const char *au_c2d_get_kerning(const AuCanvas2D *c);

typedef struct AuTextMetrics2D {
    double width;
    double left, right;          /* actualBoundingBoxLeft, Right */
    double ascent, descent;      /* actualBoundingBoxAscent, Descent */
    double font_ascent, font_descent;
    double em_ascent, em_descent;
    double hanging, alphabetic, ideographic; /* baselines, above the alignment baseline */
} AuTextMetrics2D;
void au_c2d_measure_text(AuCanvas2D *c, const char *text, size_t len, AuTextMetrics2D *out);
/* max_width < 0: none. stroke draws the glyph outlines stroked. */
void au_c2d_draw_text(AuCanvas2D *c, const char *text, size_t len, double x, double y, double max_width,
                      bool stroke);

/* ---- Images ----
 * Draws the rectangle (sx, sy, sw, sh) of the source, in the units of its
 * natural size (nat_w x nat_h; the surface may be smaller), into (dx, dy,
 * dw, dh). opaque: the source's alpha is ignored. Does not change the
 * origin-clean flag: the caller taints. */
void au_c2d_draw_image(AuCanvas2D *c, const AuSurface *src, int nat_w, int nat_h, bool opaque, double sx,
                       double sy, double sw, double sh, double dx, double dy, double dw, double dh);

/* ---- Pixels ----
 * rgba is w * h * 4 bytes, R, G, B, A, not premultiplied. Reading outside
 * the canvas gives transparent black. put writes the dirty rectangle
 * (dx, dy, dw, dh, relative to the data) of the data placed at (x, y),
 * replacing pixels: no blending, clipping, shadow or transform. */
void au_c2d_get_image_data(AuCanvas2D *c, int x, int y, int w, int h, uint8_t *rgba);
void au_c2d_put_image_data(AuCanvas2D *c, const uint8_t *rgba, int w, int h, int x, int y, int dx, int dy,
                           int dw, int dh);

/* The conversions between surface pixels (0xAARRGGBB words, premultiplied)
 * and RGBA bytes, a row at a time. Each pixel is stored as one 32-bit word
 * whose value depends on the byte order: big_endian says which word to
 * build (au_c2d_native_big_endian() for real work; a test passes the
 * other order too and reads each word's bytes back reversed). opaque: alpha reads as 255 and the
 * colour as stored. */
bool au_c2d_native_big_endian(void);
void au_c2d_row_to_rgba(const AuPixel *src, uint8_t *rgba, size_t n, bool opaque, bool big_endian);
void au_c2d_row_from_rgba(const uint8_t *rgba, AuPixel *dst, size_t n, bool big_endian);

#endif
