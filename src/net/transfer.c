/* Real downloads through the platform's files. See transfer.h. */
#include "transfer.h"

#include <string.h>

struct AuNetTransferSource {
    AuNetLoader *l;
    AuNetPageFetcher *pf;
    AuDownloadFiles files;
};

typedef struct Xfer {
    AuNetTransferSource *s;
    AuDownload *d;
    void *file;
    AuNetDownload *dl;
    uint32_t token;    /* a claimed hand-off (released, not stopped) */
    int64_t offset;    /* bytes in the file before this transfer */
    int64_t written;
    char *error;
    bool space_checked, finished;
} Xfer;

AuNetTransferSource *au_net_transfer_source_new(AuNetLoader *l, AuNetPageFetcher *pf, const AuDownloadFiles *files)
{
    AuNetTransferSource *s = au_calloc(sizeof *s);
    s->l = l;
    s->pf = pf;
    s->files = *files;
    return s;
}

void au_net_transfer_source_free(AuNetTransferSource *s)
{
    au_free(s);
}

static bool x_write(void *ctx, const void *bytes, size_t len)
{
    Xfer *x = ctx;
    if (x->error)
        return false;
    char *err = NULL;
    if (!x->s->files.write(x->s->files.ctx, x->file, bytes, len, &err)) {
        x->error = err ? err : au_strndup("cannot write the file", 21);
        return false;
    }
    x->written += (int64_t)len;
    return true;
}

static void release(Xfer *x)
{
    if (x->token && x->s->pf)
        au_net_page_fetcher_release_download(x->s->pf, x->token);
    else if (x->dl)
        au_net_download_stop(x->dl);
    x->dl = NULL;
    x->token = 0;
}

static void *src_start(void *ctx, AuDownload *d, int64_t offset, char **error)
{
    AuNetTransferSource *s = ctx;
    (void)offset; /* the partial file says how much is there */
    Xfer *x = au_calloc(sizeof *x);
    x->s = s;
    x->d = d;
    AuDownloadSink sink = {x, x_write};
    /* A page's response: continue on its transfer, from the start. */
    if (d->handoff && s->pf) {
        int64_t existing = 0;
        x->file = s->files.open(s->files.ctx, d, true, &existing, error);
        if (!x->file) {
            au_net_page_fetcher_release_download(s->pf, d->handoff);
            d->handoff = 0;
            au_free(x);
            return NULL;
        }
        char *err = NULL;
        x->dl = au_net_page_fetcher_take_download(s->pf, d->handoff, &sink, &err);
        if (x->dl) {
            x->token = d->handoff;
            d->handoff = 0;
            return x;
        }
        d->handoff = 0;
        if (err) {
            *error = err;
            s->files.close(s->files.ctx, x->file, d, false, NULL);
            au_free(x);
            return NULL;
        }
        /* Gone (the page went away first): fetch it afresh below. */
        s->files.close(s->files.ctx, x->file, d, false, NULL);
        x->file = NULL;
    }
    int64_t existing = 0;
    x->file = s->files.open(s->files.ctx, d, false, &existing, error);
    if (!x->file) {
        au_free(x);
        return NULL;
    }
    x->offset = existing;
    x->dl = au_net_download_start(s->l, d->url, existing, &sink, error);
    if (!x->dl) {
        s->files.close(s->files.ctx, x->file, d, false, NULL);
        au_free(x);
        return NULL;
    }
    return x;
}

static int64_t src_poll(void *ctx, void *handle, uint32_t now_ms, int64_t *total, bool *done, char **error)
{
    AuNetTransferSource *s = ctx;
    Xfer *x = handle;
    (void)now_ms;
    if (x->finished) {
        *done = true;
        return x->offset + x->written;
    }
    int64_t t = -1;
    bool end = false;
    char *err = NULL;
    au_net_loader_poll(s->l); /* cheap when nothing is ready; no page may be pumping */
    au_net_download_poll(x->dl, &t, &end, &err);
    if (x->error) {
        au_free(err);
        *error = au_strndup(x->error, strlen(x->error));
        return x->offset + x->written;
    }
    if (err) {
        /* A server that will not resume: start again from the start. */
        if (x->offset > 0 && !x->written && !strcmp(err, "the server cannot resume this download")) {
            au_free(err);
            release(x);
            s->files.close(s->files.ctx, x->file, x->d, false, NULL);
            int64_t existing = 0;
            char *e2 = NULL;
            x->file = s->files.open(s->files.ctx, x->d, true, &existing, &e2);
            if (!x->file) {
                *error = e2 ? e2 : au_strndup("cannot open the file", 20);
                return 0;
            }
            x->offset = 0;
            AuDownloadSink sink = {x, x_write};
            x->dl = au_net_download_start(s->l, x->d->url, 0, &sink, error);
            return 0;
        }
        *error = err;
        return x->offset + x->written;
    }
    /* Both kinds of transfer report the whole file's size (a ranged one
     * adds its offset). */
    if (t >= 0)
        *total = t;
    if (t >= 0 && !x->space_checked) {
        x->space_checked = true;
        int64_t left = t - (x->offset + x->written);
        if (left > 0 && s->files.has_space && !s->files.has_space(s->files.ctx, x->file, left)) {
            *error = au_strndup("There is not enough room on the disk", 36);
            return x->offset + x->written;
        }
    }
    if (end) {
        release(x);
        char *e = NULL;
        bool ok = s->files.close(s->files.ctx, x->file, x->d, true, &e);
        x->file = NULL;
        x->finished = true;
        if (!ok) {
            *error = e ? e : au_strndup("cannot finish the file", 22);
            return x->offset + x->written;
        }
        *done = true;
    }
    return x->offset + x->written;
}

static void src_stop(void *ctx, void *handle)
{
    AuNetTransferSource *s = ctx;
    Xfer *x = handle;
    release(x);
    if (x->file)
        s->files.close(s->files.ctx, x->file, x->d, false, NULL);
    au_free(x->error);
    au_free(x);
}

static void src_discard(void *ctx, AuDownload *d)
{
    AuNetTransferSource *s = ctx;
    if (d->handoff && s->pf)
        au_net_page_fetcher_release_download(s->pf, d->handoff);
    if (s->files.remove)
        s->files.remove(s->files.ctx, d);
}

void au_net_transfer_source_interface(AuNetTransferSource *s, AuTransferSource *out)
{
    out->ctx = s;
    out->start = src_start;
    out->poll = src_poll;
    out->stop = src_stop;
    out->discard = src_discard;
}
