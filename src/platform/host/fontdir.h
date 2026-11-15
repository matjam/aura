/* Bundled fonts on the host (fontdir.c). */
#ifndef AURA_HOST_FONTDIR_H
#define AURA_HOST_FONTDIR_H

#include "platform/platform.h"

typedef struct AuHostFontDir AuHostFontDir;

/* Registers the faces of a Fonts folder with the set, lazily: from
 * dir/fonts.idx, or by scanning dir and its subfolders for .ttf and .ttc
 * files when there is no index. flags: AU_FONT_*. The result owns the
 * loaded font data; free it after the set. */
AuHostFontDir *au_host_add_font_dir(AuFontSet *set, const char *dir, unsigned flags);
/* Faces registered, and font files read so far. */
int au_host_font_dir_count(const AuHostFontDir *d);
size_t au_host_font_dir_loads(const AuHostFontDir *d);
void au_host_font_dir_free(AuHostFontDir *d);
/* Writes the fonts.idx text for the fonts under dir. False when there are
 * none. */
bool au_host_font_index(const char *dir, AuBuf *out);

#endif
