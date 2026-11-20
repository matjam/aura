/* Where a window is on the screen, for the script properties that say so
 * (screen.width, window.screenX, outerWidth, a mouse event's screenX).
 * The shell answers AuBrowserHost.window_geometry; the layers between
 * pass it down. Portable C99.
 *
 * A browser's numbers agree with each other: the window lies on the
 * screen, its outer size holds its inner one, and a mouse event's screen
 * position is its client position plus where the page is on the screen.
 * A page that checks (hCaptcha's proof script reports all of them)
 * reads a window at (0, 0) as large as its viewport on a 1024x768 screen
 * it does not fit, whose events have screenX == clientX, as a scripted
 * one: those were the defaults before the shells answered. */
#ifndef AURA_PAGE_SCREEN_H
#define AURA_PAGE_SCREEN_H

typedef struct AuScreenGeom {
    int screen_w, screen_h;                 /* the screen the window is on */
    int avail_x, avail_y, avail_w, avail_h; /* less the menu bar and the Dock */
    int win_x, win_y, win_w, win_h;         /* the window with its frame, from the screen's top left */
    int page_x, page_y;                     /* the top left of the document's viewport, on the screen */
} AuScreenGeom;

#endif
