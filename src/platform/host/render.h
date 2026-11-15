/* Host tools for documents: the render and dump commands, and a file:
 * loader for the document view. */
#ifndef AURA_HOST_RENDER_H
#define AURA_HOST_RENDER_H

#include "page/page.h"

/* aura-host render|dump ...: argv[1] is the command. */
int au_host_render_main(int argc, char **argv);
/* aura-host image FILE ...: decodes one image (image_tool.c). */
int au_host_image_main(int argc, char **argv);

/* A loader for file: URLs. Requests are queued; au_host_loader_pump
 * delivers every queued request (data, then done) and returns true if it
 * delivered any. Images are decoded for their size only (PNG, GIF, JPEG,
 * BMP headers); the view paints a placeholder. */
typedef struct AuHostLoader AuHostLoader;
AuHostLoader *au_host_loader_new(void);
void au_host_loader_free(AuHostLoader *l);
void au_host_loader_interface(AuHostLoader *l, AuLoader *out);
bool au_host_loader_pump(AuHostLoader *l, AuDocView *dv);
/* The file path of a file: URL (percent-decoded, without query or
 * fragment). Returns false for other URLs. */
bool au_host_file_path(const char *url, char *out, size_t cap);

#endif
