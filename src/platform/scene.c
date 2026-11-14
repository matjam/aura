/* The paint test scene. See scene.h. */
#include "scene.h"

#include <stdio.h>
#include <string.h>

static void text(AuCanvas *c, AuFontFace *f, float size, float x, float y, const char *s, AuColor color)
{
    if (f)
        au_draw_text(c, f, size, x, y, s, strlen(s), color);
}

static void label(AuCanvas *c, AuFontFace *f, float x, float y, const char *s)
{
    text(c, f, 11, x, y, s, AU_RGB(90, 90, 100));
}

/* A small procedural image: a 4x4 checkerboard with a colour ramp and a
 * transparent corner, premultiplied. */
static AuSurface *make_image(void)
{
    AuSurface *img = au_surface_new(8, 8);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint32_t r = (uint32_t)(x * 255 / 7), g = (uint32_t)(y * 255 / 7), b = ((x / 2 + y / 2) & 1) ? 230 : 40;
            uint32_t a = (x + y < 3) ? 0 : 255;
            uint32_t p = a ? (a << 24) | (r << 16) | (g << 8) | b : 0;
            img->pixels[y * img->stride + x] = p;
        }
    }
    return img;
}

static void star(AuPath *p, float cx, float cy, float r)
{
    /* A five-pointed star drawn as one self-intersecting pentagram, so the
     * fill rule decides whether the centre is filled. */
    static const float pts[5][2] = {{0, -1}, {0.588f, 0.809f}, {-0.951f, -0.309f}, {0.951f, -0.309f},
                                    {-0.588f, 0.809f}};
    au_path_move_to(p, cx + pts[0][0] * r, cy + pts[0][1] * r);
    for (int i = 1; i < 5; i++)
        au_path_line_to(p, cx + pts[i][0] * r, cy + pts[i][1] * r);
    au_path_close(p);
}

void au_scene_draw(AuCanvas *c, AuFontSet *fonts, int width, int height, int scroll_y)
{
    AuFontFace *sans = au_fontset_match(fonts, "Helvetica, Arial, sans-serif", 400, false);
    AuFontFace *sans_bold = au_fontset_match(fonts, "Helvetica, Arial, sans-serif", 700, false);
    AuFontFace *serif = au_fontset_match(fonts, "'Times New Roman', Times, serif", 400, false);
    AuFontFace *serif_italic = au_fontset_match(fonts, "'Times New Roman', Times, serif", 400, true);
    AuFontFace *mono = au_fontset_match(fonts, "'Courier New', monospace", 400, false);
    AuFontFace *georgia = au_fontset_match(fonts, "Georgia, serif", 400, false);
    AuFontFace *verdana = au_fontset_match(fonts, "Verdana, sans-serif", 400, false);

    au_surface_clear(au_canvas_target(c), AU_RGB(250, 250, 247));
    au_canvas_translate(c, 0, (float)-scroll_y);
    float W = (float)width;

    /* Header band: a horizontal gradient the width of the window. */
    AuGradient g = {0};
    AuRect band = {0, 0, W, 56};
    au_gradient_css_line(&g, band, 90);
    au_gradient_add_stop(&g, 0, AU_RGB(32, 60, 120));
    au_gradient_add_stop(&g, 1, AU_RGB(120, 40, 110));
    au_fill_rect_gradient(c, band, &g);
    text(c, sans_bold, 30, 16, 38, "Aura", AU_RGB(255, 255, 255));
    text(c, sans, 13, 104, 36, "paint and font test scene", AU_RGBA(255, 255, 255, 210));

    /* Text at several sizes and families. */
    float y = 84;
    static const float sizes[] = {9, 10, 12, 14, 18, 24};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%gpx The quick brown fox jumps", sizes[i]);
        text(c, sans, sizes[i], 16, y, buf, AU_RGB(20, 20, 20));
        y += sizes[i] * 1.35f + 2;
    }
    y += 4;
    text(c, serif, 16, 16, y, "Times New Roman: Sphinx of black quartz, judge my vow.", AU_RGB(20, 20, 20));
    y += 22;
    text(c, serif_italic, 16, 16, y, "Italic: AVATAR Wavy Type, kerning: AV To Yo", AU_RGB(20, 20, 20));
    y += 22;
    text(c, mono, 14, 16, y, "Courier New: for (i = 0; i < n; i++) {}", AU_RGB(20, 20, 20));
    y += 22;
    text(c, georgia, 16, 16, y, "Georgia 0123456789 \xE2\x80\x9C" "quotes\xE2\x80\x9D \xE2\x80\x94 caf\xC3\xA9", AU_RGB(20, 20, 20));
    y += 22;
    text(c, verdana, 13, 16, y, "Verdana: Pack my box with five dozen liquor jugs.", AU_RGB(20, 20, 20));
    y += 14;

    /* Shapes. */
    float sy = y + 20;
    label(c, sans, 16, sy - 6, "paths: nonzero, even-odd, curves, stroke");
    AuPath p;
    au_path_init(&p);
    star(&p, 60, sy + 50, 42);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(220, 140, 20));
    au_path_clear(&p);
    star(&p, 160, sy + 50, 42);
    au_fill_path(c, &p, AU_FILL_EVENODD, AU_RGB(40, 140, 60));
    au_path_clear(&p);
    au_path_ellipse(&p, 260, sy + 50, 44, 30);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(40, 90, 200, 200));
    au_path_clear(&p);
    au_path_move_to(&p, 320, sy + 90);
    au_path_cubic_to(&p, 330, sy - 10, 400, sy + 110, 420, sy + 10);
    au_path_quad_to(&p, 400, sy + 100, 320, sy + 90);
    au_path_close(&p);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA(200, 30, 60, 180));
    au_path_clear(&p);
    {
        AuPath s;
        au_path_init(&s);
        au_path_move_to(&s, 440, sy + 90);
        au_path_line_to(&s, 470, sy + 10);
        au_path_line_to(&s, 500, sy + 80);
        au_path_quad_to(&s, 540, sy + 0, 580, sy + 70);
        au_path_stroke(&s, 5, &p);
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(60, 60, 70));
        au_path_free(&s);
    }
    au_path_clear(&p);

    /* Boxes: borders, radii, gradient, shadows. */
    float by = sy + 120;
    label(c, sans, 16, by - 6, "borders, radii, gradient, box-shadow");
    AuRect b1 = {16, by, 130, 90};
    AuRadii r1 = {{24, 8, 40, 0}, {24, 8, 20, 0}};
    AuBorder bd1 = {{6, 10, 6, 3}, {AU_RGB(200, 40, 40), AU_RGB(40, 160, 60), AU_RGB(40, 80, 200), AU_RGB(220, 180, 20)}};
    au_fill_rounded_rect(c, b1, &r1, AU_RGB(255, 245, 230));
    au_draw_border(c, b1, &bd1, &r1);

    AuRect b2 = {166, by, 130, 90};
    AuRadii r2 = {{45, 45, 45, 45}, {45, 45, 45, 45}};
    AuBorder bd2 = {{3, 3, 3, 3}, {AU_RGB(30, 30, 30), AU_RGB(30, 30, 30), AU_RGB(30, 30, 30), AU_RGB(30, 30, 30)}};
    AuGradient g2 = {0};
    au_gradient_css_line(&g2, b2, 135);
    au_gradient_add_stop(&g2, 0, AU_RGB(255, 200, 60));
    au_gradient_add_stop(&g2, 0.5f, AU_RGB(240, 80, 120));
    au_gradient_add_stop(&g2, 1, AU_RGB(60, 90, 220));
    au_path_rounded_rect(&p, b2, &r2, false);
    au_fill_path_gradient(c, &p, AU_FILL_NONZERO, &g2);
    au_path_clear(&p);
    au_draw_border(c, b2, &bd2, &r2);

    AuRect b3 = {326, by + 6, 120, 78};
    AuRadii r3 = {{10, 10, 10, 10}, {10, 10, 10, 10}};
    au_draw_box_shadow(c, b3, &r3, 4, 6, 16, 0, AU_RGBA(0, 0, 0, 110), false);
    au_fill_rounded_rect(c, b3, &r3, AU_RGB(255, 255, 255));
    text(c, sans, 12, b3.x + 12, b3.y + 44, "box-shadow", AU_RGB(40, 40, 40));

    AuRect b4 = {476, by + 6, 110, 78};
    au_fill_rect(c, b4, AU_RGB(235, 238, 245));
    au_draw_box_shadow(c, b4, NULL, 3, 3, 10, 0, AU_RGBA(0, 0, 60, 140), true);
    text(c, sans, 12, b4.x + 14, b4.y + 44, "inset", AU_RGB(40, 40, 40));

    /* Clipping, opacity, images. */
    float cy = by + 130;
    label(c, sans, 16, cy - 6, "clip, opacity group, image nearest / bilinear");
    AuRect clip = {16, cy, 160, 80};
    au_fill_rect(c, clip, AU_RGB(230, 240, 250));
    au_canvas_clip_push(c, clip);
    for (int i = 0; i < 6; i++) {
        au_path_clear(&p);
        au_path_ellipse(&p, 10 + i * 34.0f, cy + 40 + (i & 1 ? 25 : -20), 26, 26);
        au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGBA((uint32_t)(40 * i), 100, (uint32_t)(220 - 30 * i), 200));
    }
    text(c, serif, 22, 8, cy + 48, "Clipped text overflow", AU_RGB(0, 0, 0));
    au_canvas_clip_pop(c);

    AuRect grp = {196, cy, 130, 80};
    au_canvas_group_begin(c, grp);
    au_path_clear(&p);
    au_path_ellipse(&p, 240, cy + 40, 36, 36);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(230, 50, 50));
    au_path_clear(&p);
    au_path_ellipse(&p, 282, cy + 40, 36, 36);
    au_fill_path(c, &p, AU_FILL_NONZERO, AU_RGB(50, 90, 230));
    au_canvas_group_end(c, 0.5f);

    AuSurface *img = make_image();
    AuIRect all = {0, 0, img->width, img->height};
    au_draw_image(c, img, all, (AuRect){346, cy, 80, 80}, AU_FILTER_NEAREST, 1.0f);
    au_draw_image(c, img, all, (AuRect){436, cy, 80, 80}, AU_FILTER_BILINEAR, 1.0f);
    au_draw_image(c, img, all, (AuRect){526, cy + 20, 40, 40}, AU_FILTER_BILINEAR, 0.5f);
    au_surface_free(img);

    /* Footer: the size, so a resize shows a different scene. */
    float fy = cy + 120;
    char buf[64];
    snprintf(buf, sizeof buf, "window %d x %d", width, height);
    au_fill_rect(c, (AuRect){0, fy - 18, W, 26}, AU_RGB(232, 232, 228));
    au_fill_rect(c, (AuRect){0, fy - 18, W, 1}, AU_RGB(200, 200, 196));
    text(c, sans, 12, 16, fy, buf, AU_RGB(60, 60, 60));
    au_path_free(&p);

    au_canvas_translate(c, 0, (float)scroll_y); /* undo the scroll */
}
