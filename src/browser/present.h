/* Damage and scroll bookkeeping for a shell that keeps a browser window
 * in an offscreen surface and copies changed parts to the screen.
 *
 * To use it: pass AuBrowserHost.invalidate to au_present_invalidate and
 * AuBrowserHost.scroll_area to au_present_scroll. To show changes, call
 * au_present_begin: it moves the surface's pixels for the scrolls since
 * the last call (the shell moves the screen's the same way, or copies the
 * area again) and hands back the rectangles to paint with au_bw_paint and
 * then copy to the screen. Invalidations made while painting go into the
 * next round. */
#ifndef AURA_BROWSER_PRESENT_H
#define AURA_BROWSER_PRESENT_H

#include "../paint/paint.h"

#define AU_PRESENT_RECTS 8

typedef struct AuPresent {
    AuIRect rects[AU_PRESENT_RECTS]; /* damaged, in surface pixels after the pending shift */
    int n;
    bool shifting;      /* a shift is pending */
    AuIRect shift_area; /* the rectangle whose pixels move */
    int shift_dx, shift_dy;
} AuPresent;

/* What au_present_begin hands the shell for one round. */
typedef struct AuPresentRound {
    bool shifted;       /* the surface's pixels in area moved by (dx, dy) */
    AuIRect area;
    int dx, dy;
    AuIRect rects[AU_PRESENT_RECTS]; /* to paint, then to copy */
    int n;
} AuPresentRound;

void au_present_init(AuPresent *p);
void au_present_invalidate(AuPresent *p, AuIRect r);
/* The pixels in area move by (dx, dy). Damage already recorded inside
 * area moves with them. A move as large as the area, or a second area,
 * turns into damage. */
void au_present_scroll(AuPresent *p, AuIRect area, int dx, int dy);
bool au_present_pending(const AuPresent *p);
/* Takes the pending work: moves s's pixels when a shift is pending
 * (s may be NULL to only take it) and empties the damage list. */
void au_present_begin(AuPresent *p, AuSurface *s, AuPresentRound *r);
/* Moves the pixels of area in s by (dx, dy), clipped to area; the
 * pixels uncovered keep their old values. */
void au_present_shift_pixels(AuSurface *s, AuIRect area, int dx, int dy);

#endif
