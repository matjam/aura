/* Real downloads: the browser's AuTransferSource (downloads.h) on the
 * loader, writing through the platform's file operations.
 *
 * A download writes to a partial file beside its final name in the
 * download folder and is renamed when complete; a paused, failed or
 * interrupted download keeps the partial file, and resuming asks the
 * server for the rest with a Range request (the platform reports how many
 * bytes the partial file already holds). A server that ignores the Range
 * starts the download again from the start. A page's document response
 * handed to the download manager (AuDownload.handoff) continues on its
 * own transfer, with nothing fetched twice. The platform checks free
 * space when the size becomes known and when a write fails. Portable
 * C99; nothing here opens a file itself. */
#ifndef AURA_NET_TRANSFER_H
#define AURA_NET_TRANSFER_H

#include "loader.h"
#include "../browser/downloads.h"

/* The platform's files for downloads. Handles are the platform's. */
typedef struct AuDownloadFiles {
    void *ctx;
    /* Opens d's partial file for appending, creating it when missing;
     * *existing gets the bytes it holds. With restart set, empties it
     * first. NULL with *error (au_alloc'd) when it cannot. */
    void *(*open)(void *ctx, AuDownload *d, bool restart, int64_t *existing, char **error);
    /* Appends bytes; false with *error (au_alloc'd) when it cannot (disk
     * full...). */
    bool (*write)(void *ctx, void *file, const void *bytes, size_t len, char **error);
    /* Whether the disk has room for bytes more. Optional (true). */
    bool (*has_space)(void *ctx, void *file, int64_t bytes);
    /* Closes the file. With complete set, renames it to d's final name
     * (made unique; d->filename is updated when it changes) and sets its
     * type and creator; false with *error when that fails. */
    bool (*close)(void *ctx, void *file, AuDownload *d, bool complete, char **error);
    /* Deletes d's partial file, if any. */
    void (*remove)(void *ctx, AuDownload *d);
} AuDownloadFiles;

typedef struct AuNetTransferSource AuNetTransferSource;

/* pf may be NULL (no page hand-offs). files is copied. */
AuNetTransferSource *au_net_transfer_source_new(AuNetLoader *l, AuNetPageFetcher *pf, const AuDownloadFiles *files);
/* Call after au_downloads_free (which stops every transfer). */
void au_net_transfer_source_free(AuNetTransferSource *s);
void au_net_transfer_source_interface(AuNetTransferSource *s, AuTransferSource *out);

#endif
