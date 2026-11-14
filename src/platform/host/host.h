/* Host-only additions to the platform interface. */
#ifndef AURA_HOST_H
#define AURA_HOST_H

#include "platform/platform.h"

/* Fonts named on the command line; au_platform_load_fonts loads them
 * before the system fonts. */
void au_host_add_font_path(const char *path);
/* Whether au_platform_load_fonts also loads the macOS system fonts
 * (default true). */
void au_host_use_system_fonts(bool on);
/* The offscreen surface standing in for the window. */
void au_host_set_window_surface(AuSurface *s);

#endif
