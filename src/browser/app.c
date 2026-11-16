/* Application services: the files Aura keeps, loading them at start,
 * saving them when they change, the Running marker and session restore.
 * The network stack is in app_net.c. See app.h. */
#include "app_int.h"

#include "favicon.h"

#include "../js/clock.h"

#include <stdio.h>
#include <string.h>

/* A profile file larger than this is not read. */
#define AU_APP_FILE_MAX (16UL * 1024 * 1024)

static const char *const file_names[AU_APP_F_COUNT] = {AU_APP_FILE_PREFS, AU_APP_FILE_BOOKMARKS, AU_APP_FILE_HISTORY,
                                                       AU_APP_FILE_SESSION, AU_APP_FILE_DOWNLOADS};

/* ---- Clocks ---- */

uint32_t au_app_ms(AuApp *a) { return a->port.ms(a->port.ctx); }

int64_t au_app_wall_time(AuApp *a) { return a->port.wall_time(a->port.ctx); }

/* Script's Date takes its time and zone from the port's clock, so
 * Date.now() agrees with cookie expiry and certificate checks. Every
 * date, past or future, gets the offset of now (Mac OS 9 has no zone
 * database); it is read at most once a minute. */
static int64_t script_wall_ms(void *ctx)
{
    AuApp *a = ctx;
    return a->port.wall_ms(a->port.ctx);
}

static int32_t script_utc_offset(void *ctx, int64_t utc_ms)
{
    AuApp *a = ctx;
    (void)utc_ms;
    uint32_t now = au_app_ms(a);
    if (!a->offset_read || now - a->offset_read_ms >= 60000u) {
        a->offset = a->port.utc_offset(a->port.ctx);
        a->offset_read_ms = now;
        a->offset_read = true;
    }
    return a->offset;
}

/* ---- Files ---- */

bool au_app_read_file(const AuAppFiles *f, const char *name, char **data, size_t *len)
{
    AuBuf b;
    au_buf_init(&b);
    if (!f->read(f->ctx, NULL, name, &b, AU_APP_FILE_MAX)) {
        au_buf_free(&b);
        return false;
    }
    *data = au_buf_take(&b, len);
    return true;
}

bool au_app_write_file(const AuAppFiles *f, const char *name, const AuBuf *data)
{
    return f->write(f->ctx, NULL, name, data->data ? data->data : "", data->len);
}

bool au_app_set_aside(const AuAppFiles *f, const char *folder, const char *name)
{
    char aside[64];
    snprintf(aside, sizeof aside, "%s (damaged)", name);
    return f->rename(f->ctx, folder, name, aside);
}

/* ---- The browser's data sets ---- */

static uint32_t generation(AuApp *a, AuAppFileId id)
{
    AuBrowser *b = a->b;
    switch (id) {
    case AU_APP_F_PREFS: return b->prefs_gen;
    case AU_APP_F_BOOKMARKS: return b->bookmarks.generation;
    case AU_APP_F_HISTORY: return b->history.generation;
    case AU_APP_F_SESSION: return b->session_gen + b->scroll_gen;
    case AU_APP_F_DOWNLOADS: return b->downloads.generation;
    default: return 0;
    }
}

static void serialize(AuApp *a, AuAppFileId id, AuBuf *out)
{
    AuBrowser *b = a->b;
    switch (id) {
    case AU_APP_F_PREFS: au_prefs_serialize(&b->prefs, out); break;
    case AU_APP_F_BOOKMARKS: au_bookmarks_serialize(&b->bookmarks, out); break;
    case AU_APP_F_HISTORY:
        au_history_expire(&b->history, (AuTime)au_app_wall_time(a), b->prefs.history_days);
        au_history_serialize(&b->history, out);
        break;
    case AU_APP_F_SESSION: au_browser_session_serialize(b, out); break;
    case AU_APP_F_DOWNLOADS: au_downloads_serialize(&b->downloads, out); break;
    default: break;
    }
}

/* Every set as saved: nothing is written until it changes. */
static void mark_saved(AuApp *a)
{
    for (int i = 0; i < AU_APP_F_COUNT; i++) {
        a->save[i].saved_gen = generation(a, (AuAppFileId)i);
        a->save[i].pending = false;
    }
    a->saved_session_gen = a->b->session_gen;
}

AuApp *au_app_new(AuBrowser *b, const AuAppPort *port)
{
    AuApp *a = au_calloc(sizeof *a);
    a->b = b;
    a->port = *port;
    if (port->wall_ms && port->utc_offset) {
        AuJsClock clock = {a, script_wall_ms, script_utc_offset};
        au_js_set_clock(&clock);
        a->clock_installed = true;
    }
    mark_saved(a);
    return a;
}

void au_app_free(AuApp *a)
{
    if (!a)
        return;
    if (a->net_started)
        au_app_net_stop(a);
    if (a->clock_installed)
        au_js_set_clock(NULL);
    au_free(a);
}

/* A first run: a couple of starting points. */
static void first_bookmarks(AuApp *a)
{
    AuBookmarks *bm = &a->b->bookmarks;
    AuTime t = (AuTime)au_app_wall_time(a);
    au_bookmark_add(bm, NULL, NULL, "Aura start page", "about:home", t);
    AuBookmark *f = au_bookmark_add_folder(bm, NULL, NULL, "Mac OS 9", t);
    au_bookmark_add(bm, f, NULL, "Macintosh Garden", "http://macintoshgarden.org/", t);
    au_bookmark_add(bm, f, NULL, "Mac OS 9 Lives!", "http://macos9lives.com/", t);
    au_bookmark_add(bm, f, NULL, "FrogFind", "http://frogfind.com/", t);
}

static void load_files(AuApp *a)
{
    AuBrowser *b = a->b;
    const AuAppFiles *f = &a->port.files;
    char *data;
    size_t len;
    b->history.utc_offset = a->port.utc_offset(a->port.ctx);
    if (au_app_read_file(f, AU_APP_FILE_PREFS, &data, &len)) {
        if (!au_prefs_parse(&b->prefs, data, len))
            au_app_set_aside(f, NULL, AU_APP_FILE_PREFS);
        au_free(data);
    }
    if (au_app_read_file(f, AU_APP_FILE_BOOKMARKS, &data, &len)) {
        if (!au_bookmarks_parse(&b->bookmarks, data, len))
            au_app_set_aside(f, NULL, AU_APP_FILE_BOOKMARKS);
        au_free(data);
    } else {
        first_bookmarks(a);
    }
    if (au_app_read_file(f, AU_APP_FILE_HISTORY, &data, &len)) {
        if (!au_history_parse(&b->history, data, len))
            au_app_set_aside(f, NULL, AU_APP_FILE_HISTORY);
        au_free(data);
        au_history_expire(&b->history, (AuTime)au_app_wall_time(a), b->prefs.history_days);
    }
    if (au_app_read_file(f, AU_APP_FILE_DOWNLOADS, &data, &len)) {
        if (!au_downloads_parse(&b->downloads, data, len))
            au_app_set_aside(f, NULL, AU_APP_FILE_DOWNLOADS);
        au_free(data);
    }
    /* Site icons: a damaged file is only dropped (they are fetched
     * again). */
    if (au_app_read_file(f, AU_APP_FILE_FAVICONS, &data, &len)) {
        au_favicons_parse(b->favicons, data, len);
        au_free(data);
    }
    mark_saved(a);
}

/* ---- Unclean exits ----
 * A file "Running" exists while Aura runs: made at start, removed after
 * the last save at quit. Found at start, it means the last run crashed
 * or was forced to quit (an out-of-memory exit leaves it too). */

static bool begin_run(AuApp *a)
{
    const AuAppFiles *f = &a->port.files;
    if (f->exists(f->ctx, NULL, AU_APP_FILE_RUNNING))
        return true;
    f->write(f->ctx, NULL, AU_APP_FILE_RUNNING, "", 0);
    return false;
}

bool au_app_load(AuApp *a)
{
    load_files(a);
    return begin_run(a);
}

AuAppRestore au_app_session(AuApp *a, bool unclean, AuSession *s)
{
    const AuAppFiles *f = &a->port.files;
    bool restore = a->b->prefs.startup == AU_STARTUP_RESTORE;
    memset(s, 0, sizeof *s);
    if (!restore && !unclean)
        return AU_APP_RESTORE_NONE;
    char *data;
    size_t len;
    if (!au_app_read_file(f, AU_APP_FILE_SESSION, &data, &len))
        return AU_APP_RESTORE_NONE;
    if (!au_session_parse(s, data, len))
        au_app_set_aside(f, NULL, AU_APP_FILE_SESSION);
    au_free(data);
    if (s->nwindows <= 0) {
        au_session_free(s);
        return AU_APP_RESTORE_NONE;
    }
    return restore ? AU_APP_RESTORE_YES : AU_APP_RESTORE_ASK;
}

const char *au_app_start_url(AuApp *a)
{
    return a->b->prefs.startup == AU_STARTUP_BLANK ? "about:blank" : NULL;
}

/* ---- Saving ---- */

static void save_favicons(AuApp *a, bool force, uint32_t now)
{
    AuBrowser *b = a->b;
    /* At most every half minute. With history off they are kept for the
     * run only, and a file from before is emptied. */
    if (!au_favicons_dirty(b->favicons) || (!force && now - a->favicons_saved_ms < AU_APP_SCROLL_SAVE_DELAY_MS))
        return;
    AuBuf out;
    au_buf_init(&out);
    if (b->prefs.history_enabled) {
        au_favicons_serialize(b->favicons, &out);
    } else {
        AuFavicons *none = au_favicons_new();
        au_favicons_serialize(none, &out);
        au_favicons_free(none);
        AuBuf scratch;
        au_buf_init(&scratch);
        au_favicons_serialize(b->favicons, &scratch); /* clears dirty */
        au_buf_free(&scratch);
    }
    au_app_write_file(&a->port.files, AU_APP_FILE_FAVICONS, &out);
    a->favicons_saved_ms = now;
    au_buf_free(&out);
}

void au_app_save(AuApp *a, bool force)
{
    uint32_t now = au_app_ms(a);
    for (int i = 0; i < AU_APP_F_COUNT; i++) {
        AuAppSaveState *st = &a->save[i];
        uint32_t g = generation(a, (AuAppFileId)i);
        if (g == st->saved_gen) {
            st->pending = false;
            continue;
        }
        if (!st->pending) {
            st->pending = true;
            st->first_change_ms = now;
        }
        /* The session is saved soon after tabs change, and every half
         * minute while only scroll positions do. */
        uint32_t delay = AU_APP_SAVE_DELAY_MS;
        if (i == AU_APP_F_SESSION && a->b->session_gen == a->saved_session_gen)
            delay = AU_APP_SCROLL_SAVE_DELAY_MS;
        if (!force && now - st->first_change_ms < delay)
            continue;
        AuBuf out;
        au_buf_init(&out);
        serialize(a, (AuAppFileId)i, &out);
        if (au_app_write_file(&a->port.files, file_names[i], &out)) {
            st->saved_gen = generation(a, (AuAppFileId)i);
            if (i == AU_APP_F_SESSION)
                a->saved_session_gen = a->b->session_gen;
            st->pending = false;
        } else {
            st->first_change_ms = now; /* try again after the delay */
        }
        au_buf_free(&out);
    }
    save_favicons(a, force, now);
    if (a->net_started)
        au_app_net_save(a, force);
}

void au_app_quit(AuApp *a)
{
    AuBrowser *b = a->b;
    if (b->prefs.clear_history_on_quit) {
        au_history_clear(&b->history);
        au_favicons_clear(b->favicons);
    }
    au_app_save(a, true);
    const AuAppFiles *f = &a->port.files;
    f->remove(f->ctx, NULL, AU_APP_FILE_RUNNING);
}

/* ---- A folder as a file table ---- */

static uint32_t folder_us(AuAppFolder *f)
{
    return f->us ? f->us(f->us_ctx) : 0;
}

bool au_app_folder_init(AuAppFolder *f, const AuAppFiles *files, const char *name, size_t max)
{
    memset(f, 0, sizeof *f);
    f->files = files;
    f->name = name;
    f->max = max;
    return files->make_folder(files->ctx, name);
}

/* An empty file reads as missing. */
static bool folder_read(void *ctx, const char *name, AuBuf *out)
{
    AuAppFolder *f = ctx;
    uint32_t t0 = folder_us(f);
    size_t before = out->len;
    bool ok = f->files->read(f->files->ctx, f->name, name, out, f->max) && out->len > before;
    f->read_us += folder_us(f) - t0;
    f->reads++;
    return ok;
}

static bool folder_write(void *ctx, const char *name, const void *data, size_t len)
{
    AuAppFolder *f = ctx;
    uint32_t t0 = folder_us(f);
    bool ok = f->files->write(f->files->ctx, f->name, name, data, len);
    uint32_t dt = folder_us(f) - t0;
    f->write_us += dt;
    f->writes++;
    if (len >= f->largest_len) {
        f->largest_len = (uint32_t)len;
        f->largest_us = dt;
    }
    return ok;
}

static void folder_remove(void *ctx, const char *name)
{
    AuAppFolder *f = ctx;
    f->files->remove(f->files->ctx, f->name, name);
}

/* A file that does not parse is renamed "<name> (damaged)", or removed
 * when it cannot be. */
static void folder_damaged(void *ctx, const char *name)
{
    AuAppFolder *f = ctx;
    if (!au_app_set_aside(f->files, f->name, name))
        f->files->remove(f->files->ctx, f->name, name);
}

static void folder_list(void *ctx, void (*each)(void *, const char *, size_t, uint32_t), void *each_ctx)
{
    AuAppFolder *f = ctx;
    f->files->list(f->files->ctx, f->name, each, each_ctx);
}

void au_app_folder_web_storage(AuAppFolder *f, AuWebStorageFiles *out)
{
    out->ctx = f;
    out->read = folder_read;
    out->write = folder_write;
    out->remove = folder_remove;
    out->damaged = folder_damaged;
}

void au_app_folder_disk_cache(AuAppFolder *f, AuDiskCacheFiles *out)
{
    out->ctx = f;
    out->list = folder_list;
    out->read = folder_read;
    out->write = folder_write;
    out->remove = folder_remove;
}
