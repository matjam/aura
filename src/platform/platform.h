/* The interface between Aura and the operating system.
 *
 * Each platform (src/platform/host, src/platform/macos9) implements the
 * functions below. The engine (everything outside src/platform) never
 * calls them; the application shell does, and passes the engine what it
 * needs (a surface to paint into, a font set, events). That keeps the
 * engine testable on the host with no OS at all.
 *
 * Still to come, with room left for them here: networking (a non-blocking
 * socket API driven from the idle event), timers (a deadline the event
 * loop honours), the clipboard, and opening URLs and files from the
 * Finder. */
#ifndef AURA_PLATFORM_H
#define AURA_PLATFORM_H

#include "../paint/font.h"
#include "../paint/paint.h"

/* ---- Time ---- */

/* Milliseconds from an arbitrary start; wraps after 49 days. */
uint32_t au_platform_time_ms(void);

/* ---- Files ---- */

/* Reads a whole file into memory from au_alloc (freed with au_free).
 * Paths are platform paths (POSIX on the host, HFS paths on Mac OS 9).
 * Returns false when the file cannot be read. */
bool au_platform_read_file(const char *path, char **data, size_t *len);

/* ---- Fonts ---- */

/* Registers the system's fonts with the set and maps the generic families
 * (serif, sans-serif, monospace, system-ui) to installed ones. Font data
 * stays owned by the platform for the life of the program. */
void au_platform_load_fonts(AuFontSet *set);

/* ---- Window ---- */

/* The surface the engine paints the window's content into, sized to the
 * content area. It changes after a resize event; fetch it again then. */
AuSurface *au_platform_window_surface(void);

/* Shows a region of the window surface on screen (copies it to the
 * window). Coordinates are surface pixels. */
void au_platform_present(int x, int y, int w, int h);

/* ---- Events ----
 * The shell turns OS events into these and hands them to the engine. */

typedef enum {
    AU_EVENT_NONE,
    AU_EVENT_MOUSE_DOWN,
    AU_EVENT_MOUSE_UP,
    AU_EVENT_MOUSE_MOVE,
    AU_EVENT_KEY_DOWN,
    AU_EVENT_KEY_UP,
    AU_EVENT_SCROLL, /* dx, dy in pixels (scroll bar or wheel) */
    AU_EVENT_RESIZE, /* width, height of the new content area */
    AU_EVENT_IDLE,   /* nothing happened; do background work */
    AU_EVENT_QUIT
} AuEventType;

enum {
    AU_MOD_SHIFT = 1,
    AU_MOD_CONTROL = 2,
    AU_MOD_OPTION = 4, /* Alt */
    AU_MOD_COMMAND = 8
};

/* Keys without a character. */
enum {
    AU_KEY_NONE,
    AU_KEY_LEFT,
    AU_KEY_RIGHT,
    AU_KEY_UP,
    AU_KEY_DOWN,
    AU_KEY_PAGE_UP,
    AU_KEY_PAGE_DOWN,
    AU_KEY_HOME,
    AU_KEY_END,
    AU_KEY_BACKSPACE,
    AU_KEY_DELETE,
    AU_KEY_RETURN,
    AU_KEY_TAB,
    AU_KEY_ESCAPE
};

typedef struct AuEvent {
    AuEventType type;
    uint32_t time_ms;
    int x, y;        /* mouse position in the content area, pixels */
    int button;      /* 0 primary */
    int clicks;      /* 1 single, 2 double */
    uint32_t mods;   /* AU_MOD_* */
    uint32_t ch;     /* key: the Unicode character, or 0 */
    int key;         /* key: AU_KEY_* for keys without a character */
    int dx, dy;      /* scroll */
    int width, height; /* resize */
} AuEvent;

#endif
