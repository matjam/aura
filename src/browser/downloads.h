/* Downloads: the list of downloads, their states and progress, the file
 * they are saved in, a file name sanitizer for HFS, and the interface to
 * whatever moves the bytes. Portable C99.
 *
 * AuTransferSource moves the bytes (start a transfer, poll how many bytes
 * have arrived, stop it, discard a partial file): src/net's
 * au_net_transfer_source writes real files through the platform;
 * au_sim_source is a stand-in that pretends to receive data at a
 * modem-to-Ethernet rate (View > Debug > Start Simulated Downloads).
 * Nothing is written to disk by this code; the source owns the data.
 *
 * The shell calls au_downloads_idle from the event loop. It starts queued
 * items (up to max_active at once), polls active ones and keeps a rate
 * estimate for "time left".
 *
 * File format (compacted on save; the state of an item that was queued
 * or active is written as paused, so after a restart it comes back
 * paused and marked interrupted):
 *     # Aura downloads 1
 *     <id>\t<state>\t<received>\t<total>\t<started>\t<url>\t<file>\t<mime>\t<error>
 * with state one of queued, active, paused, done, failed, cancelled and
 * total -1 when unknown. */
#ifndef AURA_BROWSER_DOWNLOADS_H
#define AURA_BROWSER_DOWNLOADS_H

#include "history.h"

#include "../paint/font.h"

typedef enum {
    AU_DL_QUEUED,
    AU_DL_ACTIVE,
    AU_DL_PAUSED,
    AU_DL_DONE,
    AU_DL_FAILED,
    AU_DL_CANCELLED
} AuDownloadState;

typedef struct AuDownload {
    uint32_t id;
    char *url;
    char *filename; /* sanitized, in the download folder */
    char *mime;
    char *error;    /* "" unless failed or interrupted */
    int64_t received;
    int64_t total;  /* -1 when unknown */
    AuDownloadState state;
    AuTime started;
    bool interrupted; /* was active when Aura last quit */
    /* Running state, not saved. */
    void *transfer;
    uint32_t handoff; /* a page's response handed to downloads (the fetcher's token); 0 for none */
    uint32_t rate;    /* bytes per second, smoothed */
    uint32_t rate_ms; /* time of the last rate sample */
    int64_t rate_bytes;
} AuDownload;

/* What moves the bytes. */
typedef struct AuTransferSource {
    void *ctx;
    /* Starts (or resumes at offset) the transfer of d; returns a handle,
     * or NULL with *error set (au_alloc'd) on failure. */
    void *(*start)(void *ctx, AuDownload *d, int64_t offset, char **error);
    /* Bytes received so far in all (including offset). Updates *total
     * when the size becomes known; sets *done at the end, or *error
     * (au_alloc'd) on failure. */
    int64_t (*poll)(void *ctx, void *handle, uint32_t now_ms, int64_t *total, bool *done, char **error);
    /* Stops the transfer; a partial file stays for a later resume. */
    void (*stop)(void *ctx, void *handle);
    /* Optional: d was cancelled or removed before it finished: delete its
     * partial file and release a handed-off response not yet claimed. */
    void (*discard)(void *ctx, AuDownload *d);
} AuTransferSource;

typedef struct AuDownloads {
    AuDownload **items; /* oldest first */
    int n, cap;
    uint32_t next_id;
    uint32_t generation; /* bumped on every change worth saving or showing */
    int max_active;
    AuTransferSource src;
} AuDownloads;

void au_downloads_init(AuDownloads *dl, const AuTransferSource *src);
void au_downloads_free(AuDownloads *dl); /* stops running transfers */

/* Queues a download. filename is sanitized (au_hfs_sanitize, without the
 * uniqueness rule: the shell applies that with its own exists check
 * before the transfer writes anything). total may be -1. */
AuDownload *au_downloads_add(AuDownloads *dl, const char *url, const char *filename, const char *mime,
                             int64_t total, AuTime now);
AuDownload *au_downloads_find(AuDownloads *dl, uint32_t id);
/* Sets progress directly (the transfer source path does this itself). */
void au_download_progress(AuDownloads *dl, AuDownload *d, int64_t received, int64_t total);
void au_download_pause(AuDownloads *dl, AuDownload *d);
void au_download_resume(AuDownloads *dl, AuDownload *d); /* paused or failed: queued again */
void au_download_cancel(AuDownloads *dl, AuDownload *d);
void au_download_remove(AuDownloads *dl, AuDownload *d);
/* Removes done, failed and cancelled items. */
void au_downloads_clear_completed(AuDownloads *dl);
/* Starts, polls and finishes transfers. */
void au_downloads_idle(AuDownloads *dl, uint32_t now_ms);
bool au_downloads_busy(const AuDownloads *dl); /* anything queued or active */

/* Estimated seconds left, or -1 when unknown. */
long au_download_eta(const AuDownload *d);
const char *au_download_state_name(AuDownloadState s);
/* "1.2 MB", "340 KB", "512 bytes". */
void au_format_size(int64_t bytes, char *out, size_t cap);

void au_downloads_serialize(const AuDownloads *dl, AuBuf *out);
/* Replaces the list with the file's. Returns false for a damaged file
 * (good lines are kept). */
bool au_downloads_parse(AuDownloads *dl, const char *data, size_t len);

/* ---- HFS file names ----
 * Makes name safe for an HFS file name: at most 31 characters (Mac
 * Roman is one byte per character; characters outside it count as one
 * and the platform maps them), ':' replaced by '-', control characters
 * dropped, a leading '.' replaced by '_' (Mac OS reserves it for
 * drivers), empty names become "download". The extension (from the last
 * '.', up to 8 characters) is kept when the name is cut. When exists is
 * not NULL and says the name is taken, " 2", " 3"... is inserted before
 * the extension until it is free. out receives UTF-8 (cap >= 128). */
void au_hfs_sanitize(const char *name, bool (*exists)(void *ctx, const char *name), void *ctx, char *out,
                     size_t cap);
/* A file name from a URL: the last path segment, percent-decoded, or the
 * host when the path ends in '/'. */
void au_download_name_from_url(const char *url, char *out, size_t cap);

/* ---- The simulated transfer source ----
 * Sizes come from a hash of the URL (40 KB to 3 MB), some sizes are
 * withheld (unknown total), URLs containing "fail" fail half way, and the
 * rate varies between 20 and 120 KB/s. */
const AuTransferSource *au_sim_source(void);

/* ---- The Downloads window's list ---- */
/* The status line of a download: "1.2 MB of 3 MB at 40 KB/s, 0:45
 * left", "Waiting", "Stopped, ...", "Done, 3 MB", "Failed: ...". */
void au_download_status(const AuDownload *d, char *out, size_t cap);
#define AU_DL_ROW_H 40
/* Paints rows into (0, 0, width, height) of the canvas, scrolled by
 * scroll pixels, with row `selected` highlighted. */
void au_downloads_paint(const AuDownloads *dl, AuCanvas *c, AuFontSet *fonts, int width, int height, int scroll,
                        int selected, bool active_window);
/* The row at y (list coordinates, scroll applied), or -1. */
int au_downloads_hit(const AuDownloads *dl, int y, int scroll);

#endif
