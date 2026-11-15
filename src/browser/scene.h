/* The paint test scene: text at several sizes, shapes, rounded borders, a
 * gradient, shadows, clipping, an opacity group and scaled images. The host
 * tool writes it to a PNG and the Mac OS 9 application draws it in its
 * window, so the two can be compared. It uses only the engine's paint and
 * font API. */
#ifndef AURA_SCENE_H
#define AURA_SCENE_H

#include "../paint/font.h"

/* The height the scene needs; the width adapts (at least 600 looks
 * right). */
#define AU_SCENE_HEIGHT 720

/* Fills the rectangle (0, 0, width, height) in canvas coordinates with
 * the scene, scrolled up by scroll_y pixels. The browser's about:scene
 * page draws it this way inside the page area. */
void au_scene_draw(AuCanvas *c, AuFontSet *fonts, int width, int height, int scroll_y);

#endif
