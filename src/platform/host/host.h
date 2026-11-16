/* Host-only additions to the platform interface, for the aura-host tool
 * and the tests that use its platform. The OS services (coroutines,
 * files, sockets, font files) are src/platform/posix's, declared in
 * posix.h. */
#ifndef AURA_HOST_H
#define AURA_HOST_H

#include "platform/platform.h"
#include "platform/posix/posix.h"

/* Fonts named on the command line; au_platform_load_fonts loads them
 * before the system fonts. */
void au_host_add_font_path(const char *path);
/* Whether au_platform_load_fonts also loads the macOS system fonts
 * (default true). */
void au_host_use_system_fonts(bool on);
/* Whether it also registers the bundled fonts of the Fonts folder
 * (default true; after the system fonts). With the system fonts off, a
 * render uses only the bundled set, as a Mac OS 9 machine without the
 * system's fonts would. */
void au_host_use_bundled_fonts(bool on);
/* The Fonts folder: set here, else $AURA_FONTS, else assets/fonts of the
 * source tree. */
void au_host_set_fonts_dir(const char *dir);
const char *au_host_fonts_dir(void);
/* Reads a font option at argv[i] (--font F, --no-system-fonts,
 * --bundled-only, --no-bundled-fonts, --fonts-dir D). Returns the
 * arguments it used, 0 when argv[i] is not one. */
int au_host_font_option(int argc, char **argv, int i);
/* The offscreen surface standing in for the window. */
void au_host_set_window_surface(AuSurface *s);
/* aura-host js (jsrun.c). */
int au_host_js_main(int argc, char **argv);
int au_host_page_main(int argc, char **argv);
/* aura-host chrome: the developer tools' --do actions (devtools_host.c);
 * false when a is not one. net may be NULL. */
struct AuBrowserWindow;
struct AuNet;
bool au_host_dev_action(struct AuBrowserWindow *w, struct AuNet *net, const char *a);

#endif
