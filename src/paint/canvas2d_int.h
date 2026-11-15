/* Internals of the 2D canvas context, shared by canvas2d.c (objects and
 * state), canvas2d_path.c (path building) and canvas2d_draw.c (drawing).
 * Not for use outside src/paint. */
#ifndef AURA_CANVAS2D_INT_H
#define AURA_CANVAS2D_INT_H

#include "canvas2d.h"
#include "paint_internal.h"

#define C2D_COORD_MAX 900000.0f
#define C2D_FONT_CACHE 8

/* Composite operators, in the order of the name table (canvas2d.c). The
 * first eleven are Porter-Duff, the rest blend modes. */
enum {
    OP_SOURCE_OVER, OP_SOURCE_IN, OP_SOURCE_OUT, OP_SOURCE_ATOP, OP_DEST_OVER, OP_DEST_IN, OP_DEST_OUT,
    OP_DEST_ATOP, OP_LIGHTER, OP_COPY, OP_XOR,
    OP_MULTIPLY, OP_SCREEN, OP_OVERLAY, OP_DARKEN, OP_LIGHTEN, OP_COLOR_DODGE, OP_COLOR_BURN, OP_HARD_LIGHT,
    OP_SOFT_LIGHT, OP_DIFFERENCE, OP_EXCLUSION, OP_HUE, OP_SATURATION, OP_COLOR, OP_LUMINOSITY,
    OP_COUNT
};

enum { GRAD_LINEAR, GRAD_RADIAL, GRAD_CONIC };

struct AuCanvasGradient {
    uint32_t refs;
    uint8_t kind;
    float x0, y0, r0, x1, y1, r1; /* conic: x0, y0 the centre, r0 the start angle */
    uint16_t nstops, stops_cap;
    struct C2DStop {
        float pos;
        AuColor color;
    } *stops;
    bool lut_ok;
    uint32_t *lut; /* 256 premultiplied colours, made when first drawn */
    void *user;
};

struct AuCanvasPattern {
    uint32_t refs;
    AuCanvasBitmap *bmp;
    uint8_t repeat;
    AuMatrix m;
    void *user;
};

/* A clip mask: one coverage byte per canvas pixel. */
typedef struct C2DClip {
    uint32_t refs;
    AuCanvasSet *set; /* for the byte count; NULL once the set is gone */
    size_t bytes;
    uint8_t mask[];
} C2DClip;

typedef struct C2DFont {
    uint32_t refs;
    char *families;
    char *css;
    int weight;
    bool italic;
    float size;
} C2DFont;

typedef struct C2DStyle {
    uint8_t kind;
    AuColor color;
    AuCanvasGradient *grad;
    AuCanvasPattern *pat;
} C2DStyle;

typedef struct C2DState {
    AuMatrix m;
    C2DStyle style[2];
    float alpha;
    uint8_t op;
    float line_width, miter, dash_offset;
    uint8_t cap, join;
    uint8_t ndash;
    float dash[AU_STROKE_MAX_DASHES];
    AuColor shadow_color;
    float shadow_blur, shadow_x, shadow_y;
    C2DFont *font;
    uint8_t align, baseline, direction, kerning;
    float spacing[2];
    bool smoothing;
    uint8_t smoothing_quality;
    /* The clip: nothing draws outside clip_box; clip (when not NULL)
     * scales coverage inside it. */
    AuIRect clip_box;
    C2DClip *clip;
} C2DState;

struct AuCanvas2D {
    AuCanvasSet *set;
    AuCanvas2D *prev, *next;
    void *owner;
    int w, h;
    bool alpha, inert, clean;
    AuSurface *surf;
    AuCanvas *pc; /* a paint canvas over surf, for au_draw_image */
    bool dirty;
    AuIRect dirty_box;
    C2DState *st; /* st[nst - 1] is current */
    int nst, st_cap;
    AuCanvasPath path; /* device space */
    AuPath tmp, tmp2;  /* scratch, kept to avoid allocation */
    AuGlyphRun run;
};

struct AuCanvasSet {
    size_t budget, bytes;
    AuCanvas2D *canvases;
    AuCanvasBitmap *bitmaps;
    bool dirty;
    AuFontSet *fonts;
    struct {
        C2DFont *font; /* referenced */
        AuFontList *list;
        uint32_t used;
    } fc[C2D_FONT_CACHE];
    uint32_t fc_clock;
};

static inline C2DState *c2d_state(const AuCanvas2D *c) { return &c->st[c->nst - 1]; }

/* canvas2d.c */
bool c2d_ensure_surface(AuCanvas2D *c);
void c2d_touch(AuCanvas2D *c, AuIRect r);
void c2d_copy_pixels(AuPixel *d, const AuPixel *s, size_t n);
void c2d_zero_pixels(AuPixel *d, size_t n);
const uint32_t *c2d_gradient_lut(AuCanvasGradient *g);
AuFontList *c2d_font_list(AuCanvas2D *c);
bool c2d_charge(AuCanvasSet *s, size_t bytes);
void c2d_uncharge(AuCanvasSet *s, size_t bytes);
C2DClip *c2d_clip_new(AuCanvas2D *c);
void c2d_clip_unref(C2DClip *k);

/* canvas2d_path.c */
/* Maps (x, y) through m (NULL: identity) and clamps; false when the
 * result is not a number. */
bool c2d_map(const AuMatrix *m, double x, double y, float *ox, float *oy);
/* Whether p is an axis-aligned rectangle on whole pixels. */
bool c2d_path_irect(const AuPath *p, AuIRect *out);

/* border.c */
void au_blur_mask(uint8_t *m, int w, int h, float sigma);
/* path.c, stroke.c: what flattening and stroking would produce. */
size_t au_path_edge_count(const AuPath *p);
size_t au_path_stroke_points(const AuPath *p, float tol);

#endif
